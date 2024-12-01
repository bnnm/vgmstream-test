#ifdef _MSC_VER
#define _CRT_SECURE_NO_DEPRECATE
#endif

#include <stdio.h>
#include <io.h>

#include <foobar2000/SDK/foobar2000.h>

#include "foo_vgmstream.h"

/* Value can be adjusted freely but 8k is a good enough compromise. */
#define STREAMFILE_DEFAULT_BUFFER_SIZE 0x8000


/* a STREAMFILE that operates via foobar's file service using a buffer */
typedef struct {
    bool m_file_opened;         /* if foobar IO service opened the file */
    service_ptr_t<file> m_file; /* foobar IO service */
    abort_callback* p_abort;    /* foobar error stuff */
    /*const*/ char* name;       /* IO filename */
    int name_len;               /* cache */

    char* archname;             /* for foobar's foo_unpack archives */
    int archname_len;           /* cache */
    int archpath_end;           /* where the last \ ends before archive name */
    int archfile_end;           /* where the last | ends before file name */

    int64_t offset;             /* last read offset (info) */
    int64_t buf_offset;         /* current buffer data start */
    uint8_t* buf;               /* data buffer */
    size_t buf_size;            /* max buffer size */
    size_t valid_size;          /* current buffer size */
    size_t file_size;           /* buffered file size */
} foo_streamfile_t;

static libstreamfile_t* open_foo_streamfile_internal(const char* const filename, abort_callback* p_abort, t_filestats* stats);
static libstreamfile_t* open_foo_streamfile_from_file(service_ptr_t<file> m_file, bool m_file_opened, const char* const filename, abort_callback* p_abort);

static int foo_read(void* user_data, uint8_t* dst, int dst_size) {
    foo_streamfile_t* sf = (foo_streamfile_t*)user_data;
    if (!sf || !dst)
        return 0;

    size_t read_total = 0;
    if (!sf || !sf->m_file_opened || !dst || dst_size <= 0)
        return 0;

    /* is the part of the requested length in the buffer? */
    if (sf->offset >= sf->buf_offset && sf->offset < sf->buf_offset + sf->valid_size) {
        size_t buf_limit;
        int buf_into = (int)(sf->offset - sf->buf_offset);

        buf_limit = sf->valid_size - buf_into;
        if (buf_limit > dst_size)
            buf_limit = dst_size;

        memcpy(dst, sf->buf + buf_into, buf_limit);
        read_total += buf_limit;
        dst_size -= buf_limit;
        sf->offset += buf_limit;
        dst += buf_limit;
    }


    /* read the rest of the requested length */
    while (dst_size > 0) {
        size_t buf_limit;

        /* ignore requests at EOF */
        if (sf->offset >= sf->file_size) {
            //offset = sf->file_size; /* seems fseek doesn't clamp offset */
            //VGM_ASSERT_ONCE(offset > sf->file_size, "STDIO: reading over file_size 0x%x @ 0x%lx + 0x%x\n", sf->file_size, offset, length);
            break;
        }

        /* position to new offset */
        try {
            sf->m_file->seek(sf->offset, *sf->p_abort);
        } catch (...) {
            break; /* this shouldn't happen in our code */
        }

        /* fill the buffer (offset now is beyond buf_offset) */
        try {
            sf->buf_offset = sf->offset;
            sf->valid_size = sf->m_file->read(sf->buf, sf->buf_size, *sf->p_abort);
        } catch(...) {
            break; /* improbable? */
        }

        /* decide how much must be read this time */
        if (dst_size > sf->buf_size)
            buf_limit = sf->buf_size;
        else
            buf_limit = dst_size;

        /* give up on partial reads (EOF) */
        if (sf->valid_size < buf_limit) {
            memcpy(dst, sf->buf, sf->valid_size);
            sf->offset += sf->valid_size;
            read_total += sf->valid_size;
            break;
        }

        /* use the new buffer */
        memcpy(dst, sf->buf, buf_limit);
        sf->offset += buf_limit;
        read_total += buf_limit;
        dst_size -= buf_limit;
        dst += buf_limit;
    }

    return read_total;
}

static int64_t foo_seek(void* user_data, int64_t offset, int whence) {
    foo_streamfile_t* sf = (foo_streamfile_t*)user_data;
    if (!sf)
        return -1;

    switch (whence) {
        case LIBSTREAMFILE_SEEK_SET: /* absolute */
            break;
        case LIBSTREAMFILE_SEEK_CUR: /* relative to current */
            offset += sf->offset;
            break;
        case LIBSTREAMFILE_SEEK_END: /* relative to file end (should be negative) */
            offset += sf->file_size;
            break;
        default:
            break;
    }

    /* clamp offset like fseek */
    if (offset > sf->file_size)
        offset = sf->file_size;
    else if (offset < 0)
        offset = 0;

    /* main seek */
    sf->offset = offset;
    return 0;
}

static int64_t foo_get_size(void* user_data) {
    foo_streamfile_t* sf = (foo_streamfile_t*)user_data;
    if (!sf)
        return 0;
    return sf->file_size;
}

static const char* foo_get_name(void* user_data) {
    foo_streamfile_t* sf = (foo_streamfile_t*)user_data;
    if (!sf)
        return NULL;

    return sf->name;
}

static libstreamfile_t* foo_open(void* user_data, const char* const filename) {
    foo_streamfile_t* sf = (foo_streamfile_t*)user_data;

    if (!sf || !filename)
        return NULL;

    // vgmstream may need to open "files based on another" (like a changing extension) and "files in the same subdir" (like .txth)
    // or read "base filename" to do comparison. When dealing with archives (foo_unpack plugin) the later two cases would fail, since
    // vgmstream doesn't separate the  "|" special notation foo_unpack adds.
    // To fix this, when this SF is part of an archive we give vgmstream the name without | and restore the archive on open
    // - get name:      "unpack://zip|23|file://C:\file.zip|subfile.adpcm"
    // > returns:       "unpack://zip|23|file://C:\subfile.adpcm" (otherwise base name would be "file.zip|subfile.adpcm")
    // - try opening    "unpack://zip|23|file://C:\.txth
    // > opens:         "unpack://zip|23|file://C:\file.zip|.txth
    // (assumes archives won't need to open files outside archives, and goes before filedup trick)
    if (sf->archname) {
        char finalname[FOO_PATH_LIMIT];
        const char* filepart = NULL; 

        // newly open files should be "(current-path)\newfile" or "(current-path)\folder\newfile", so we need to make
        // (archive-path = current-path)\(rest = newfile plus new folders)

        int filename_len = strlen(filename);
        if (filename_len > sf->archpath_end) {
            filepart = &filename[sf->archpath_end];
        } else  {
            filepart = strrchr(filename, '\\'); // vgmstream shouldn't remove paths though
            if (!filepart)
                filepart = filename;
            else
                filepart += 1;
        }

        //TODO improve str ops

        int filepart_len = strlen(filepart);
        if (sf->archfile_end + filepart_len + 1 >= sizeof(finalname))
            return NULL;
        // copy current path+archive ("unpack://zip|23|file://C:\file.zip|")
        memcpy(finalname, sf->archname, sf->archfile_end);
        // concat possible extra dirs and filename ("unpack://zip|23|file://C:\file.zip|" + "folder/bgm01.vag")
        memcpy(finalname + sf->archfile_end, filepart, filepart_len);
        finalname[sf->archfile_end + filepart_len] = '\0';

        // normalize subfolders inside archives to use "/" (path\archive.ext|subfolder/file.ext)
        for (int i = sf->archfile_end; i < sizeof(finalname); i++) {
            if (finalname[i] == '\0')
                break;
            if (finalname[i] == '\\')
                finalname[i] = '/';
        }

        ;console::formatter() << "finalname: " << finalname;
        return open_foo_streamfile_internal(finalname, sf->p_abort, NULL);
    }

    // if same name, duplicate the file pointer we already have open
    if (sf->m_file_opened && !strcmp(sf->name, filename)) {
        service_ptr_t<file> m_file;

        m_file = sf->m_file; //copy?
        {
            libstreamfile_t* new_sf = open_foo_streamfile_from_file(m_file, sf->m_file_opened, filename, sf->p_abort);
            if (new_sf) {
                return new_sf;
            }
            // failure, close it and try the default path (which will probably fail a second time)
        }
    }

    // a normal open, open a new file
    return open_foo_streamfile_internal(filename, sf->p_abort, NULL);
}

static void foo_close(libstreamfile_t* libsf) {
    if (!libsf)
        return;

    foo_streamfile_t* sf = (foo_streamfile_t*)libsf->user_data;
    if (sf) {
        sf->m_file.release(); //release alloc'ed ptr
        free(sf->name);
        free(sf->archname);
        free(sf->buf);
    }
    free(sf);
    free(libsf);
}


static libstreamfile_t* open_foo_streamfile_from_file(service_ptr_t<file> m_file, bool m_file_opened, const char* const filename, abort_callback* p_abort) {
    libstreamfile_t* this_sf;
    const int buf_size = STREAMFILE_DEFAULT_BUFFER_SIZE;

    this_sf = (libstreamfile_t*)calloc(1, sizeof(libstreamfile_t));
    if (!this_sf) goto fail;

    // ... (*)(...) > ... (__cdecl*)(...)
    this_sf->read = (int (*)(void*, uint8_t*, int)) foo_read;
    this_sf->seek = (int64_t (*)(void*, int64_t, int)) foo_seek;
    this_sf->get_size = (int64_t (*)(void*)) foo_get_size;
    this_sf->get_name = (const char* (*)(void*)) foo_get_name;
    this_sf->open = (libstreamfile_t* (*)(void* ,const char* const)) foo_open;
    this_sf->close = (void (*)(libstreamfile_t*)) foo_close;

    this_sf->user_data = (foo_streamfile_t*)calloc(1, sizeof(foo_streamfile_t));
    if (!this_sf->user_data) goto fail;

    foo_streamfile_t* sf = (foo_streamfile_t*)this_sf->user_data;
    sf->m_file_opened = m_file_opened;
    sf->m_file = m_file;
    sf->p_abort = p_abort;
    sf->buf_size = buf_size;
    sf->buf = (uint8_t*)calloc(buf_size, sizeof(uint8_t));
    if (!sf->buf) goto fail;

    //TODO: foobar filenames look like "file://C:\path\to\file.adx"
    // maybe should hide the internal protocol and restore on open?
    sf->name = strdup(filename);
    if (!sf->name)  goto fail;
    sf->name_len = strlen(sf->name);

    // foobar supports .zip/7z/etc archives directly, in this format: "unpack://zip|(number))|file://C:\path\to\file.zip|subfile.adx"
    // Detect if current is inside archive, so when trying to get filename or open companion files it's handled correctly    
    // Subfolders have inside the archive use / instead or / (path\archive.zip|subfolder/file)
    if (strncmp(filename, "unpack", 6) == 0) {
        const char* archfile_ptr = strrchr(sf->name, '|');
        if (archfile_ptr)
            sf->archfile_end = (int)((intptr_t)archfile_ptr + 1 - (intptr_t)sf->name); // after "|""

        const char* archpath_ptr = strrchr(sf->name, '\\');
        if (archpath_ptr)
            sf->archpath_end = (int)((intptr_t)archpath_ptr + 1 - (intptr_t)sf->name); // after "\\"

        if (sf->archpath_end <= 0 || sf->archfile_end <= 0 || sf->archpath_end > sf->archfile_end || 
                sf->archfile_end > sf->name_len || sf->archfile_end >= FOO_PATH_LIMIT) {
            // ???
            sf->archpath_end = 0;
            sf->archfile_end = 0;
        }
        else {
            sf->archname = strdup(filename);
            if (!sf->archname)  goto fail;
            sf->archname_len = sf->name_len;
            int copy_len = strlen(&sf->archname[sf->archfile_end]);

            // change from "(path)\\(archive)|(filename)" to "(path)\\filename)" (smaller so shouldn't overrun)
            memcpy(sf->name + sf->archpath_end, sf->archname + sf->archfile_end, copy_len);
            sf->name[sf->archpath_end + copy_len] = '\0';

            //;console::formatter() << "base name: " << sf->name;
        }
    }

    /* cache file_size */
    if (sf->m_file_opened)
        sf->file_size = sf->m_file->get_size(*sf->p_abort);
    else
        sf->file_size = 0;

    /* STDIO has an optimization to close unneeded FDs if file size is less than buffer,
     * but seems foobar doesn't need this (reuses FDs?) */

    return this_sf;

fail:
    foo_close(this_sf);
    return NULL;
}

static libstreamfile_t* open_foo_streamfile_internal(const char* const filename, abort_callback* p_abort, t_filestats* stats) {
    libstreamfile_t* sf = NULL;
    service_ptr_t<file> infile;
    bool infile_exists;

    try {
        infile_exists = filesystem::g_exists(filename, *p_abort);
        if (!infile_exists) {
            /* allow non-existing files in some cases */
            if (!libvgmstream_is_virtual_filename(filename))
                return NULL;
        }

        if (infile_exists) {
            filesystem::g_open_read(infile, filename, *p_abort);
            if(stats) *stats = infile->get_stats(*p_abort);
        }
        
        sf = open_foo_streamfile_from_file(infile, infile_exists, filename, p_abort);
        if (!sf) {
            //m_file.release(); //refcounted and cleaned after it goes out of scope
        }

    } catch (...) {
        /* somehow foobar2000 throws an exception on g_exists when filename has a double \
         * (traditionally Windows treats that like a single slash and fopen handles it fine) */
        return NULL;
    }

    return sf;
}

libstreamfile_t* open_foo_streamfile(const char* const filename, abort_callback* p_abort, t_filestats* stats) {
    return open_foo_streamfile_internal(filename, p_abort, stats);
}
