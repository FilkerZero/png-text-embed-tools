#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#if defined(_WIN32)
/* I refuse to corrupt the build with Winsock2 library requirement just to get network
 * byte order functions because it makes the Makefile not work on Linux
 */
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define __BSWAP32__(x) (((uint32_t)(x) << 24) | (((uint32_t)(x) & 0x0000FF00) << 8) \
                        | (((uint32_t)(x) >> 8) & 0x0000FF00) | ((uint32_t)(x) >> 24))
#define htonl __BSWAP32__
#define ntohl __BSWAP32__
#elif __BYTE_ORDER == __BIG_ENDIAN
#define htonl(x) (x)
#define ntohl(x) (x)
#else
#error You're running on a Windows based toolchain that doesn't set __BYTE_ORDER in a way I understand
#endif /* endian check */
#else
#include <arpa/inet.h>
#endif

extern uint32_t crc_calculate(char*, int);

#define PNG_SIG_SIZE 8
const char* const png_sig = "\211\120\116\107\015\012\032\012";

static void inject_text_chunk(const char *key,
			      const char *content,
			      int outFd)
{
   static const char txtChunk[4] = "tEXt";
   uint32_t crc;
   uint32_t payloadLen;
   uint32_t length;
   uint32_t keyLen;
   uint32_t contentLen;
   uint32_t tmp;
   char*    buffer;
   char*    next;
   
   keyLen = strlen(key);
   contentLen = strlen(content);

   /* the chunk to write to the output consists of:
    * - the chunk payload data length (4 bytes) not included in the CRC or the length value
    * - the chunk name (4 bytes), included in CRC, not included in the length value
    * - the chunk payload data, included in the CRC and it is what the length field includes
    *    - the key (up to 79 bytes)
    *    - a NULL (1 byte),
    *    - the string data (arbitrary),
    * - the chunk CRC (4 bytes), covering from the chunk name through the end of the payload
    */
   payloadLen = keyLen + sizeof(uint8_t) + contentLen;
   length = sizeof length + sizeof txtChunk + payloadLen + sizeof crc;

   buffer = (char *)malloc(length);
   if (buffer == NULL)
   {
      fprintf(stderr, "Unable to allocate %u bytes for the text chunk\n", length);
      return;
   }
   tmp = htonl(payloadLen);
   memcpy(buffer, &tmp, sizeof tmp);
   next = buffer + sizeof tmp;
   memcpy(next, txtChunk, sizeof txtChunk);
   next += sizeof txtChunk;
   memcpy(next, key, keyLen);
   next += keyLen;
   *next++ = '\0';
   memcpy(next, content, contentLen);
   next += contentLen;
   /* CRC excludes the length bytes and itself */
   crc = htonl(crc_calculate(buffer + sizeof length, sizeof txtChunk + payloadLen));
   memcpy(next, (uint8_t*)&crc, sizeof crc);
   write(outFd, buffer, length);
   free(buffer);
}


int usage(const char* argv0)
{
   fprintf(stderr, "Usage: %s <in> <out> <key> <content>\n\n", argv0);
   fprintf(stderr, "Reads a PNG image from <in> ('-' for stdin)\n");
   fprintf(stderr, "appends a text chunk (key=content)\n");
   fprintf(stderr, "copies all other chunks unchanged to <out> ('-' for stdout)\n");
}   

int main(int argc, char *argv[])
{
   int inFd = -1;
   int outFd = -1;
   int buf_size = 1024;		// Initial value
   char* buffer;
   int inserted = 0;
   int chunkCount = 0;
   char sig[PNG_SIG_SIZE];
   ssize_t sigLen;

   /* using "progname input.png output keyname content" */
   if (argc != 5)
   {
      exit(usage(argv[0]));
   }

   /* First argument is input file name, or '-' for stdin */
   if ((strlen(argv[1]) == 1) && (argv[1][0] == '-'))
   {
      inFd = fileno(stdin);
   }
   else
   {
      inFd = open(argv[1], O_RDONLY);
   }

   if (inFd < 0)
   {
      fprintf(stderr, "unable to open input file \"%s\"\n", argv[1]);
      exit(1);
   }

   if ((strlen(argv[2]) == 1) && (argv[2][0] == '-'))
   {
      outFd = fileno(stdout);
   }
   else
   {
      outFd = open(argv[2], O_WRONLY | O_TRUNC | O_CREAT);
   }

   if (outFd < 0)
   {
      fprintf(stderr, "unable to open output file \"%s\"\n", argv[2]);
      exit(1);
   }
   

   sigLen = read(inFd, sig, sizeof sig);
   if (sigLen != PNG_SIG_SIZE)
   {
      fprintf(stderr, "assertion failure: PNG_SIG_SIZE=%d, sigLen=%ld\n", PNG_SIG_SIZE, sigLen);
      exit(1);
   }

   if (memcmp(png_sig, sig, PNG_SIG_SIZE) != 0)
   {
      fprintf(stderr, "assertion failure: Bad PNG signature: [%02x,%02x,%02x,%02x], expected [%02x,%02x,%02x,%02x]\n",
	      sig[0], sig[1], sig[2], sig[3], png_sig[0], png_sig[1], png_sig[2], png_sig[3]);
      exit(1);
   }

   write(outFd, sig, sigLen);

   buffer = malloc(buf_size);


   /* copy chunks until we find the IDAT chunk header */
   while (1)
   {
      uint32_t chunkLen;
      uint32_t crc;
      uint32_t chunkCRC;
      char* next;
      uint32_t* lenPtr;
      char* typePtr;
      char* payloadPtr;
      char* crcPtr;
      int readLen;

      readLen = read(inFd, buffer, sizeof chunkLen + 4);
      if (readLen != sizeof chunkLen + 4)
      {
	 fprintf(stderr, "Chunk header read error at %ld in %s, wanted %lu, got %d\n",
		 lseek(inFd, 0, SEEK_CUR), argv[1], sizeof chunkLen + 4, readLen);
	 exit(1);
      }

      lenPtr = (uint32_t*)buffer;
      chunkLen = ntohl(*lenPtr);

      if (buf_size < chunkLen + 3 * sizeof crc)
      {
	 buf_size = chunkLen + 3 * sizeof crc;
	 buffer = realloc(buffer, buf_size);
      }

      if (buffer == NULL)
      {
	 fprintf(stderr, "Unable to extend buffer for chunk of %u bytes\n", chunkLen);
	 exit(1);
      }
      
      typePtr = buffer + sizeof chunkLen;
      payloadPtr = &typePtr[4];
      readLen = read(inFd, payloadPtr, chunkLen + sizeof crc);
      if (readLen != chunkLen + sizeof crc)
      {
	 fprintf(stderr, "Chunk payload read error at %ld in %s, wanted %lu, got %d\n",
		 lseek(inFd, 0, SEEK_CUR), argv[1], chunkLen + sizeof crc, readLen);
	 exit(1);
      }


      /* calculate the CRC over the chunk name and payload ony */
      crc = crc_calculate(typePtr, chunkLen + 4);
      /* get the CRC from the end of the buffer */
      memcpy(&chunkCRC, payloadPtr + chunkLen, sizeof chunkCRC);
      if (ntohl(chunkCRC) != crc)
      {
	 fprintf(stderr, "Bad CRC on chunk ending at %ld of file %s, calc=%u, file=%u\n",
		 lseek(inFd, 0, SEEK_CUR), argv[1], crc, ntohl(chunkCRC));
	 exit(1);
      }

      /* At this point, we know that the buffer holds a valid chunk from the input */

      if ((inserted == 0) &&
	  ((memcmp(typePtr, "IDAT", 4) == 0) || (memcmp(typePtr, "IEND", 4) == 0)))
      {
	 /* Insert the new chunk before writing the file chunk back out */
	 inject_text_chunk(argv[3], argv[4], outFd);
	 inserted = chunkCount;
	 chunkCount++;
      }
      
      /* Write the pending chunk read from the input to the output */
      readLen = write(outFd, buffer, chunkLen + 3 * sizeof(chunkLen));
      if (readLen != chunkLen + 3 * sizeof chunkLen)
      {
	 fprintf(stderr, "Failed to write chunk of %lu bytes to output (\"%s\")\n",
		 chunkLen + 3 * sizeof chunkLen, argv[2]);
	 exit(1);
      }
      chunkCount++;
      if (memcmp(typePtr, "IEND", 4) == 0)
      { /* just wrote the last chunk */
	 break; /* get out of the loop */
      }
   }

   /* clean up */
   if (inFd != fileno(stdin))
   {
      close(inFd);
   }

   if (outFd != fileno(stdout))
   {
      close(outFd);
      printf("Wrote %d chunks to \"%s\", new tEXt chunk inserted at chunk %u\n",
	     chunkCount, argv[2], inserted);
   }

   free(buffer);

   exit(0);
}

