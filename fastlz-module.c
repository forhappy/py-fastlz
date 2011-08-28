/*
 * =====================================================================================
 *
 *       Filename:  fastlz-module.c
 *
 *    Description:  python binding for fastlz compression library
 *
 *        Version:  1.0
 *        Created:  08/25/2011 12:17:41 PM
 *       Revision:  r1
 *       Compiler:  gcc
 *
 *         Author:  Fu Haiping <email:haipingf@gmail.com>
 *        Company:  ICT
 *
 * =====================================================================================
 */

/*
 * fastlz's python binding
 * Copyright (C) 2011 Fu Haiping(email:haipingf@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * */
#define FASTLZ_PYTHON_MODULE_VERSION "0.1"
#include <Python.h>
#include "fastlz.h"
#define DEBUG

/*
 * Ensure we have the updated fastlz version
 * */
#if !defined(PY_VERSION_HEX) || (PY_VERSION_HEX < 0X010502F0)
# error "Need Python version 1.5.2 or higher!"
#endif
#if (FASTLZ_VERSION_MAJOR < 0 && FASTLZ_VERSION_MINOR < 0)
# error "Need quicklz version 0.1 or higher!"
#endif

static PyObject *FastlzError;


#undef PATH_SEPARATOR

#if defined(MSDOS) || defined(__MSDOS__) || defined(MSDOS)
#define PATH_SEPARATOR '\\'
#endif

#if defined(WIN32) || defined(__NT__) || defined(_WIN32) || defined(__WIN32__)
#define PATH_SEPARATOR '\\'
#if defined(__BORLANDC__) || defined(_MSC_VER)
#define inline __inline
#endif
#endif

#ifndef PATH_SEPARATOR
#define PATH_SEPARATOR '/'
#endif

#undef SIXPACK_BENCHMARK_WIN32
#if defined(WIN32) || defined(__NT__) || defined(_WIN32) || defined(__WIN32__)
#if defined(_MSC_VER) || defined(__GNUC__)
#define SIXPACK_BENCHMARK_WIN32
#include <windows.h>
#endif
#endif

/* magic identifier for 6pack file */
static unsigned char sixpack_magic[8] = {137, '6', 'P', 'K', 13, 10, 26, 10};

#define BLOCK_SIZE (2*64*1024)

/* prototypes */
static inline unsigned long update_adler32(unsigned long checksum, const void *buf, int len);
int detect_magic(FILE *f);
void write_magic(FILE *f);
void write_chunk_header(FILE* f, int id, int options, unsigned long size,
                        unsigned long checksum, unsigned long extra);
unsigned long block_compress(const unsigned char* input, unsigned long length, unsigned char* output);
int pack_file_compressed(const char* input_file, int method, int level, FILE* f);
int pack_file(int compress_level, const char* input_file, const char* output_file);

/* for Adler-32 checksum algorithm, see RFC 1950 Section 8.2 */
#define ADLER32_BASE 65521
static inline unsigned long update_adler32(unsigned long checksum, const void *buf, int len)
{
    const unsigned char* ptr = (const unsigned char*)buf;
    unsigned long s1 = checksum & 0xffff;
    unsigned long s2 = (checksum >> 16) & 0xffff;

    while(len>0)
    {
        unsigned k = len < 5552 ? len : 5552;
        len -= k;

        while(k >= 8)
        {
            s1 += *ptr++;
            s2 += s1;
            s1 += *ptr++;
            s2 += s1;
            s1 += *ptr++;
            s2 += s1;
            s1 += *ptr++;
            s2 += s1;
            s1 += *ptr++;
            s2 += s1;
            s1 += *ptr++;
            s2 += s1;
            s1 += *ptr++;
            s2 += s1;
            s1 += *ptr++;
            s2 += s1;
            k -= 8;
        }

        while(k-- > 0)
        {
            s1 += *ptr++;
            s2 += s1;
        }
        s1 = s1 % ADLER32_BASE;
        s2 = s2 % ADLER32_BASE;
    }
    return (s2 << 16) + s1;
}


/* return non-zero if magic sequence is detected */
/* warning: reset the read pointer to the beginning of the file */
int detect_magic(FILE *f)
{
    unsigned char buffer[8];
    size_t bytes_read;
    int c;

    fseek(f, SEEK_SET, 0);
    bytes_read = fread(buffer, 1, 8, f);
    fseek(f, SEEK_SET, 0);
    if(bytes_read < 8)
        return 0;

    for(c = 0; c < 8; c++)
        if(buffer[c] != sixpack_magic[c])
            return 0;

    return -1;
}

void write_magic(FILE *f)
{
    fwrite(sixpack_magic, 8, 1, f);
}

void write_chunk_header(FILE* f, int id, int options, unsigned long size,
                        unsigned long checksum, unsigned long extra)
{
    unsigned char buffer[16];

    buffer[0] = id & 255;
    buffer[1] = id >> 8;
    buffer[2] = options & 255;
    buffer[3] = options >> 8;
    buffer[4] = size & 255;
    buffer[5] = (size >> 8) & 255;
    buffer[6] = (size >> 16) & 255;
    buffer[7] = (size >> 24) & 255;
    buffer[8] = checksum & 255;
    buffer[9] = (checksum >> 8) & 255;
    buffer[10] = (checksum >> 16) & 255;
    buffer[11] = (checksum >> 24) & 255;
    buffer[12] = extra & 255;
    buffer[13] = (extra >> 8) & 255;
    buffer[14] = (extra >> 16) & 255;
    buffer[15] = (extra >> 24) & 255;

    fwrite(buffer, 16, 1, f);
}

int pack_file_compressed(const char* input_file, int method, int level, FILE* f)
{
    FILE* in;
    unsigned long fsize;
    unsigned long checksum;
    const char* shown_name;
    unsigned char buffer[BLOCK_SIZE];
    unsigned char result[BLOCK_SIZE*2]; /* FIXME twice is too large */
    unsigned char progress[20];
    int c;
    unsigned long percent;
    unsigned long total_read;
    unsigned long total_compressed;
    int chunk_size;

    /* sanity check */
    in = fopen(input_file, "rb");
    if (!in) {
        printf("Error: could not open %s\n", input_file);
        return -1;
    }

    /* find size of the file */
    fseek(in, 0, SEEK_END);
    fsize = ftell(in);
    fseek(in, 0, SEEK_SET);

    /* already a 6pack archive? */
    if (detect_magic(in)) {
        printf("Error: file %s is already a 6pack archive!\n", input_file);
        fclose(in);
        return -1;
    }

    /* truncate directory prefix, e.g. "foo/bar/FILE.txt" becomes "FILE.txt" */
    shown_name = input_file + strlen(input_file) - 1;
    while(shown_name > input_file)
        if(*(shown_name-1) == PATH_SEPARATOR)
            break;
        else
            shown_name--;

    /* chunk for File Entry */
    buffer[0] = fsize & 255;
    buffer[1] = (fsize >> 8) & 255;
    buffer[2] = (fsize >> 16) & 255;
    buffer[3] = (fsize >> 24) & 255;
#if 0
    buffer[4] = (fsize >> 32) & 255;
    buffer[5] = (fsize >> 40) & 255;
    buffer[6] = (fsize >> 48) & 255;
    buffer[7] = (fsize >> 56) & 255;
#else
    /* because fsize is only 32-bit */
    buffer[4] = 0;
    buffer[5] = 0;
    buffer[6] = 0;
    buffer[7] = 0;
#endif
    buffer[8] = (strlen(shown_name)+1) & 255;
    buffer[9] = (strlen(shown_name)+1) >> 8;
    checksum = 1L;
    checksum = update_adler32(checksum, buffer, 10);
    checksum = update_adler32(checksum, shown_name, strlen(shown_name)+1);
    write_chunk_header(f, 1, 0, 10+strlen(shown_name)+1, checksum, 0);
    fwrite(buffer, 10, 1, f);
    fwrite(shown_name, strlen(shown_name)+1, 1, f);
    total_compressed = 16 + 10 + strlen(shown_name)+1;

    /* for progress status */
    memset(progress, ' ', 20);
    if(strlen(shown_name) < 16)
        for(c = 0; c < (int)strlen(shown_name); c++)
            progress[c] = shown_name[c];
    else {
        for(c = 0; c < 13; c++)
            progress[c] = shown_name[c];
        progress[13] = '.';
        progress[14] = '.';
        progress[15] = ' ';
    }
    progress[16] = '[';
    progress[17] = 0;
    printf("%s", progress);
    for(c = 0; c < 50; c++)
        printf(".");
    printf("]\r");
    printf("%s", progress);

    /* read file and place in archive */
    total_read = 0;
    percent = 0;
    for(;;)
    {
        int compress_method = method;
        int last_percent = (int)percent;
        size_t bytes_read = fread(buffer, 1, BLOCK_SIZE, in);
        if(bytes_read == 0)
            break;
        total_read += bytes_read;

        /* for progress */
        if(fsize < (1<<24))
            percent = total_read * 100 / fsize;
        else
            percent = total_read/256 * 100 / (fsize >>8);
        percent >>= 1;
        while(last_percent < (int)percent)
        {
            printf("#");
            last_percent++;
        }

        /* too small, don't bother to compress */
        if(bytes_read < 32)
            compress_method = 0;

        /* write to output */
        switch(compress_method)
        {
            /* FastLZ */
        case 1:
            chunk_size = fastlz_compress_level(level, buffer, bytes_read, result);
            checksum = update_adler32(1L, result, chunk_size);
            write_chunk_header(f, 17, 1, chunk_size, checksum, bytes_read);
            fwrite(result, 1, chunk_size, f);
            total_compressed += 16;
            total_compressed += chunk_size;
            break;

            /* uncompressed, also fallback method */
        case 0:
        default:
            checksum = 1L;
            checksum = update_adler32(checksum, buffer, bytes_read);
            write_chunk_header(f, 17, 0, bytes_read, checksum, bytes_read);
            fwrite(buffer, 1, bytes_read, f);
            total_compressed += 16;
            total_compressed += bytes_read;
            break;
        }
    }

    fclose(in);
    if(total_read != fsize)
    {
        printf("\n");
        printf("Error: reading %s failed!\n", input_file);
        return -1;
    }
    else
    {
        printf("] ");
        if(total_compressed < fsize)
        {
            if(fsize < (1<<20))
                percent = total_compressed * 1000 / fsize;
            else
                percent = total_compressed/256 * 1000 / (fsize >>8);
            percent = 1000 - percent;
            printf("%2d.%d%% saved", (int)percent/10, (int)percent%10);
        }
        printf("\n");
    }

    return 0;
}

int pack_file(int compress_level, const char* input_file, const char* output_file)
{
    FILE* f;
    int result;
    f = fopen(output_file, "rb");
    if (f) {
        fclose(f);
        printf("Error: file %s already exists. Aborted.\n\n", output_file);
        return -1;
    }
    f = fopen(output_file, "wb");
    if (!f) {
        printf("Error: could not create %s. Aborted.\n\n", output_file);
        return -1;
    }
    write_magic(f);
    result = pack_file_compressed(input_file, 1, compress_level, f);
    fclose(f);
    return result;
}


#if defined(WIN32) || defined(__NT__) || defined(_WIN32) || defined(__WIN32__)
#if defined(__BORLANDC__) || defined(_MSC_VER)
#define inline __inline
#endif
#endif



/* prototypes */
static inline unsigned long readU16(const unsigned char* ptr);
static inline unsigned long readU32(const unsigned char* ptr);
void read_chunk_header(FILE* f, int* id, int* options, unsigned long* size,
                       unsigned long* checksum, unsigned long* extra);
int unpack_file(const char* archive_file);



static inline unsigned long readU16( const unsigned char* ptr )
{
    return ptr[0]+(ptr[1]<<8);
}

static inline unsigned long readU32( const unsigned char* ptr )
{
    return ptr[0]+(ptr[1]<<8)+(ptr[2]<<16)+(ptr[3]<<24);
}

void read_chunk_header(FILE* f, int* id, int* options, unsigned long* size,
                       unsigned long* checksum, unsigned long* extra)
{
    unsigned char buffer[16];
    fread(buffer, 1, 16, f);

    *id = readU16(buffer) & 0xffff;
    *options = readU16(buffer+2) & 0xffff;
    *size = readU32(buffer+4) & 0xffffffff;
    *checksum = readU32(buffer+8) & 0xffffffff;
    *extra = readU32(buffer+12) & 0xffffffff;
}

int unpack_file(const char* input_file)
{
    FILE* in;
    unsigned long fsize;
    int c;
    unsigned long percent;
    unsigned char progress[20];
    int chunk_id;
    int chunk_options;
    unsigned long chunk_size;
    unsigned long chunk_checksum;
    unsigned long chunk_extra;
    unsigned char buffer[BLOCK_SIZE];
    unsigned long checksum;

    unsigned long decompressed_size;
    unsigned long total_extracted;
    int name_length;
    char* output_file;
    FILE* f;

    unsigned char* compressed_buffer;
    unsigned char* decompressed_buffer;
    unsigned long compressed_bufsize;
    unsigned long decompressed_bufsize;

    /* sanity check */
    in = fopen(input_file, "rb");
    if(!in)
    {
        printf("Error: could not open %s\n", input_file);
        return -1;
    }

    /* find size of the file */
    fseek(in, 0, SEEK_END);
    fsize = ftell(in);
    fseek(in, 0, SEEK_SET);

    /* not a 6pack archive? */
    if(!detect_magic(in))
    {
        fclose(in);
        printf("Error: file %s is not a 6pack archive!\n", input_file);
        return -1;
    }

    printf("Archive: %s", input_file);

    /* position of first chunk */
    fseek(in, 8, SEEK_SET);

    /* initialize */
    output_file = 0;
    f = 0;
    total_extracted = 0;
    decompressed_size = 0;
    percent = 0;
    compressed_buffer = 0;
    decompressed_buffer = 0;
    compressed_bufsize = 0;
    decompressed_bufsize = 0;

    /* main loop */
    for(;;)
    {
        /* end of file? */
        size_t pos = ftell(in);
        if(pos >= fsize)
            break;

        read_chunk_header(in, &chunk_id, &chunk_options,
                          &chunk_size, &chunk_checksum, &chunk_extra);

        if((chunk_id == 1) && (chunk_size > 10) && (chunk_size < BLOCK_SIZE))
        {
            /* close current file, if any */
            printf("\n");
            free(output_file);
            output_file = 0;
            if(f)
                fclose(f);

            /* file entry */
            fread(buffer, 1, chunk_size, in);
            checksum = update_adler32(1L, buffer, chunk_size);
            if(checksum != chunk_checksum)
            {
                free(output_file);
                output_file = 0;
                fclose(in);
                printf("\nError: checksum mismatch!\n");
                printf("Got %08lX Expecting %08lX\n", checksum, chunk_checksum);
                return -1;
            }

            decompressed_size = readU32(buffer);
            total_extracted = 0;
            percent = 0;

            /* get file to extract */
            name_length = (int)readU16(buffer+8);
            if(name_length > (int)chunk_size - 10)
                name_length = chunk_size - 10;
            output_file = (char*)malloc(name_length+1);
            memset(output_file, 0, name_length+1);
            for(c = 0; c < name_length; c++)
                output_file[c] = buffer[10+c];

            /* check if already exists */
            f = fopen(output_file, "rb");
            if(f)
            {
                fclose(f);
                printf("File %s already exists. Skipped.\n", output_file);
                free(output_file);
                output_file = 0;
                f = 0;
            }
            else
            {
                /* create the file */
                f = fopen(output_file, "wb");
                if(!f)
                {
                    printf("Can't create file %s. Skipped.\n", output_file);
                    free(output_file);
                    output_file = 0;
                    f = 0;
                }
                else
                {
                    /* for progress status */
                    printf("\n");
                    memset(progress, ' ', 20);
                    if(strlen(output_file) < 16)
                        for(c = 0; c < (int)strlen(output_file); c++)
                            progress[c] = output_file[c];
                    else
                    {
                        for(c = 0; c < 13; c++)
                            progress[c] = output_file[c];
                        progress[13] = '.';
                        progress[14] = '.';
                        progress[15] = ' ';
                    }
                    progress[16] = '[';
                    progress[17] = 0;
                    printf("%s", progress);
                    for(c = 0; c < 50; c++)
                        printf(".");
                    printf("]\r");
                    printf("%s", progress);
                }
            }
        }

        if((chunk_id == 17) && f && output_file && decompressed_size)
        {
            unsigned long remaining;

            /* uncompressed */
            switch(chunk_options)
            {
                /* stored, simply copy to output */
            case 0:
                /* read one block at at time, write and update checksum */
                total_extracted += chunk_size;
                remaining = chunk_size;
                checksum = 1L;
                for(;;)
                {
                    unsigned long r = (BLOCK_SIZE < remaining) ? BLOCK_SIZE: remaining;
                    size_t bytes_read = fread(buffer, 1, r, in);
                    if(bytes_read == 0)
                        break;
                    fwrite(buffer, 1, bytes_read, f);
                    checksum = update_adler32(checksum, buffer, bytes_read);
                    remaining -= bytes_read;
                }

                /* verify everything is written correctly */
                if(checksum != chunk_checksum)
                {
                    fclose(f);
                    f = 0;
                    free(output_file);
                    output_file = 0;
                    printf("\nError: checksum mismatch. Aborted.\n");
                    printf("Got %08lX Expecting %08lX\n", checksum, chunk_checksum);
                }
                break;

                /* compressed using FastLZ */
            case 1:
                /* enlarge input buffer if necessary */
                if(chunk_size > compressed_bufsize)
                {
                    compressed_bufsize = chunk_size;
                    free(compressed_buffer);
                    compressed_buffer = (unsigned char*)malloc(compressed_bufsize);
                }

                /* enlarge output buffer if necessary */
                if(chunk_extra > decompressed_bufsize)
                {
                    decompressed_bufsize = chunk_extra;
                    free(decompressed_buffer);
                    decompressed_buffer = (unsigned char*)malloc(decompressed_bufsize);
                }

                /* read and check checksum */
                fread(compressed_buffer, 1, chunk_size, in);
                checksum = update_adler32(1L, compressed_buffer, chunk_size);
                total_extracted += chunk_extra;

                /* verify that the chunk data is correct */
                if(checksum != chunk_checksum)
                {
                    fclose(f);
                    f = 0;
                    free(output_file);
                    output_file = 0;
                    printf("\nError: checksum mismatch. Skipped.\n");
                    printf("Got %08lX Expecting %08lX\n", checksum, chunk_checksum);
                }
                else
                {
                    /* decompress and verify */
                    remaining = fastlz_decompress(compressed_buffer, chunk_size, decompressed_buffer, chunk_extra);
                    if(remaining != chunk_extra)
                    {
                        fclose(f);
                        f = 0;
                        free(output_file);
                        output_file = 0;
                        printf("\nError: decompression failed. Skipped.\n");
                    }
                    else
                        fwrite(decompressed_buffer, 1, chunk_extra, f);
                }
                break;

            default:
                printf("\nError: unknown compression method (%d)\n", chunk_options);
                fclose(f);
                f = 0;
                free(output_file);
                output_file = 0;
                break;
            }

            /* for progress, if everything is fine */
            if(f)
            {
                int last_percent = (int)percent;
                if(decompressed_size < (1<<24))
                    percent = total_extracted * 100 / decompressed_size;
                else
                    percent = total_extracted / 256 * 100 / (decompressed_size >>8);
                percent >>= 1;
                while(last_percent < (int)percent)
                {
                    printf("#");
                    last_percent++;
                }
            }
        }

        /* position of next chunk */
        fseek(in, pos + 16 + chunk_size, SEEK_SET);
    }
    printf("\n\n");

    /* free allocated stuff */
    free(compressed_buffer);
    free(decompressed_buffer);
    free(output_file);

    /* close working files */
    if(f)
        fclose(f);
    fclose(in);

    /* so far so good */
    return 0;
}
/*
 * Compress a block of data in the input buffer and returns the size of
 * compressed block. The size of input buffer is specified by length. The
 * minimum input buffer size is 16.
 *
 * The output buffer must be at least 5% larger than the input buffer
 * and can not be smaller than 66 bytes.
 *
 * If the input is not compressible, the return value might be larger than
 * length (input buffer size).
 *
 * The input buffer and the output buffer can not overlap.
 *
 *
 * int fastlz_compress(const void* input, int length, void* output);
 *
 *
 * */

/*
 * Compress a block of data in the input buffer and returns the size of
 * compressed block. The size of input buffer is specified by length. The
 * minimum input buffer size is 16.
 *
 * The output buffer must be at least 5% larger than the input buffer
 * and can not be smaller than 66 bytes.
 *
 * If the input is not compressible, the return value might be larger than
 * length (input buffer size).
 *
 * The input buffer and the output buffer can not overlap.
 *
 * Compression level can be specified in parameter level. At the moment,
 * only level 1 and level 2 are supported.
 *
 * Level 1 is the fastest compression and generally useful for short data.
 * Level 2 is slightly slower but it gives better compression ratio.
 *
 * Note that the compressed data, regardless of the level, can always be
 * decompressed using the function fastlz_decompress above.
 *
 *
 * int fastlz_compress_level(int level, const void* input, int length, void* output);
 *
 * */

static char fastlz_compress_doc[] =
    "compress(string) -- Compress string using the default compression level,"
    "returning a new string containing the compressed data.\n\n"
    "compress(string, level) -- Compress string, using the chosen compression "
    "level(currently only level 1 and level 2 are supported). \n"
    "\tLevel 1 is fastest compression and generally useful for short data.\n"
    "\tLevel 2 is slightly slower but it gives better compression ratio.\n"
    "and returning a new string containing the compressed data.\n"
    ;

static PyObject *
compress(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;
    const char *input = NULL;
    unsigned char * output = NULL;
    int level = -1;
    int length;
    int osize;
    if (!PyArg_ParseTuple(args, "s#|i", &input, &length, &level))
        return NULL;
#if defined(DEBUG)
	printf("input : %s, length : %d, level : %d\n", input, length, level);
#endif
    if (length < 0)
        return NULL;
    output = (unsigned char *) malloc(sizeof(unsigned char)
                                      * (length * 6 / 5));

    if (output == NULL)
        return NULL;

    if ((level != 1) || (level != 2)) {
#if defined(DEBUG)
		printf("in compressing\n");
        osize = fastlz_compress(input, length, output);
		printf("osize : %d\n", osize);
#endif
    } else {
#if defined(DEBUG)
        osize = fastlz_compress_level(level, input, length, output);
		printf("osize : %d\n", osize);
#endif
	}
    result = Py_BuildValue("s#", output, osize);
    free(output);
    return result;
}
/*
 * Decompress a block of compressed data and returns the size of the
 * decompressed block. If error occurs, e.g. the compressed data is
 * corrupted or the output buffer is not large enough, then 0 (zero)
 * will be returned instead.
 *
 * The input buffer and the output buffer can not overlap.
 *
 * Decompression is memory safe and guaranteed not to write the output buffer
 * more than what is specified in maxout.
 *
 *
 * int fastlz_decompress(const void* input, int length, void* output, int maxout);
 *
 *
 * */

static char fastlz_decompress_doc[] =
    "decompress(string) -- Decompress the string and returning the decompressed data.\n "
    ;
static PyObject *
decompress(PyObject *self, PyObject *args)
{
    PyObject *result;
    const char *input;
    unsigned int isize;
    unsigned char *output;
    unsigned int osize;
    if (!PyArg_ParseTuple(args, "s#", &input, &isize))
        return NULL;
#if defined(DEBUG)
	printf("input : %s, isize : %d\n", input, isize);
#endif
    output = (unsigned char *)malloc(sizeof(unsigned char)
                                     *(isize * 6 / 5));
    if (output == NULL) {
        return NULL;
    }
#if defined(DEBUG)
	printf("in decompressing\n");
    osize = fastlz_decompress(input, isize, output, isize * 6 / 5);
	printf("osize : %d\n", osize);
#endif
    result = Py_BuildValue("s#", output, osize);
    return result;
}

static PyObject *
_pack_file(PyObject *self, PyObject *args)
{
    int level;
    const char *input_file;
    const char *output_file;
    if (!PyArg_ParseTuple(args, "iss", &level, &input_file, &output_file))
        return NULL;
    pack_file(level, input_file, output_file);
    return NULL;
}
static PyObject *
_unpack_file(PyObject *self, PyObject *args)
{

    const char *archive_file;
    if (!PyArg_ParseTuple(args, "s", &archive_file))
        return NULL;
    unpack_file(archive_file);
    return NULL;
}
static PyMethodDef fastlz_methods[] =
{
    {"compress",             (PyCFunction)compress, METH_VARARGS, fastlz_compress_doc},
    {"decompress",           (PyCFunction)decompress, METH_VARARGS, fastlz_decompress_doc},
    {"pack_file",            (PyCFunction)_pack_file, METH_VARARGS, NULL},
    {"unpack_file",          (PyCFunction)_unpack_file, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static char fastlz_doc[] =
    "The functions in this module allow compression and decompression"
    "using the fastlz library.\n\n"
    "compress(string) -- Compress a string, and returning a new string containing the compressed data.\n"
    "decompress(string) -- Decompress a string , and returning a new string containing the decompressed data.\n"
    ;

PyMODINIT_FUNC
initfastlz(void)
{
    PyObject *m, *dict, *v;
    m = Py_InitModule3("fastlz", fastlz_methods, fastlz_doc);
    if (m == NULL)
        return;

    dict = PyModule_GetDict(m);


    v = PyString_FromString("Fu Haiping <email:haipingf@gmail.com>");
    PyDict_SetItemString(dict, "__author__", v);
    Py_DECREF(v);
}

