/*
 * The code is derived from cURL example and paster.c base code.
 * The cURL example is at URL:
 * https://curl.haxx.se/libcurl/c/getinmemory.html
 * Copyright (C) 1998 - 2018, Daniel Stenberg, <daniel@haxx.se>, et al..
 *
 * The paster.c code is 
 * Copyright 2013 Patrick Lam, <p23lam@uwaterloo.ca>.
 *
 * Modifications to the code are
 * Copyright 2018-2019, Yiqing Huang, <yqhuang@uwaterloo.ca>.
 * 
 * This software may be freely redistributed under the terms of the X11 license.
 */

/** 
 * @file main_wirte_read_cb.c
 * @brief cURL write call back to save received data in a user defined memory first
 *        and then write the data to a file for verification purpose.
 *        cURL header call back extracts data sequence number from header.
 * @see https://curl.haxx.se/libcurl/c/getinmemory.html
 * @see https://curl.haxx.se/libcurl/using/
 * @see https://ec.haxx.se/callback-write.html
 */ 


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdint.h> 
#include <unistd.h>
#include <curl/curl.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>

//for catpng 
#include "crc.h"
#include "zutil.h"
#include <limits.h>
#include <dirent.h>
#include <stdbool.h>
#include <arpa/inet.h>

#define IMG_URL "http://ece252-1.uwaterloo.ca:2520/image?img=1"
#define DUM_URL "https://example.com/"
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define BUF_INC  524288   /* 1024*512  = 0.5M */

#define max(a, b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

typedef struct recv_buf2 {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;


size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
int write_file(const char *folder, const char *filename, const void *in, size_t len);

//For catpng 
void catpng(char** filePaths, int n);
int get_width_height_from_IHDR(char *filepath, int* width);
U8* get_IDAT_Data(char* filepath, int length);
int get_IDAT_length(char* filepath); 

int image_strips_have[49] = {0};


struct thread_args              /* thread input parameters struct */
{
    char url[256];
};

/**
 * @brief  cURL header call back function to extract image sequence number from 
 *         http header data. An example header for image part n (assume n = 2) is:
 *         X-Ece252-Fragment: 2
 * @param  char *p_recv: header data delivered by cURL
 * @param  size_t size size of each memb
 * @param  size_t nmemb number of memb
 * @param  void *userdata user defined data structurea
 * @return size of header data received.
 * @details this routine will be invoked multiple times by the libcurl until the full
 * header data are received.  we are only interested in the ECE252_HEADER line 
 * received so that we can extract the image sequence number from it. This
 * explains the if block in the code.
 */
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;
    
    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

        /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));

    }
    return realsize;
}


/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv, 
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;
 
    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */ 
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);   
        char *q = realloc(p->buf, new_size);
        if (q == NULL) {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}


int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;
    
    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);
    if (p == NULL) {
	return 2;
    }
    
    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1;              /* valid seq should be non-negative */
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL) {
	return 1;
    }
    
    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}


/**
 * @brief output data in memory to a file
 * @param path const char *, output file path
 * @param in  void *, input data to be written to the file
 * @param len size_t, length of the input data in bytes
 */

int write_file(const char *folder, const char *filename, const void *in, size_t len)
{

    char path[256]; 

    if (mkdir(folder, 0777) != 0 && errno != EEXIST) {
        perror("mkdir");
        return -4;
    }

    snprintf(path, sizeof(path), "%s/%s", folder, filename);

    if (path == NULL) {
        fprintf(stderr, "write_file: file name is null!\n");
        return -1;
    }

    if (in == NULL) {
        fprintf(stderr, "write_file: input data is null!\n");
        return -1;
    }

    int fd;
    FILE *fp;

    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    fp = fdopen(fd, "w");

    if (flock(fd, LOCK_EX | LOCK_NB) == -1){
        // File is in use so return
        fclose(fp);
        return -3;
    }

    if (fp == NULL) {
        perror("fopen");
        return -2;
    }

    if (fwrite(in, 1, len, fp) != len) {
        fprintf(stderr, "write_file: imcomplete write!\n");
        flock(fd, LOCK_UN);
        fclose(fp);
        return -3; 
    }
    flock(fd, LOCK_UN);
    return fclose(fp);
}

void *get_image_strip( void *arg ) 
{
    CURL *curl_handle;
    CURLcode res;
    char fname[256];
    pid_t pid = getpid();


    struct thread_args *p_in = arg;
    int result = 0;
    char *url = (char *)p_in->url;

    RECV_BUF recv_buf;
    recv_buf_init(&recv_buf, BUF_SIZE);

    /* init a curl session */
    curl_handle = curl_easy_init();

    if (curl_handle == NULL) {
        result = -1;
        goto cleanup;
    }

    /* specify URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);

    /* register write call back function to process received data */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&recv_buf);

    /* register header call back function to process received header data */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)&recv_buf);

    /* some servers requires a user-agent field */
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    /* get it! */
    res = curl_easy_perform(curl_handle);

    if( res != CURLE_OK) {
        result = -1; 
        goto cleanup;
    } 

    // We already have this image strip
    if (image_strips_have[recv_buf.seq] == 1){
        result = recv_buf.seq; 
        goto cleanup;
    }

    sprintf(fname, "./output_%d.png", recv_buf.seq);
    write_file("image_strips", fname, recv_buf.buf, recv_buf.size);
    result = recv_buf.seq;

cleanup:
    /* cleaning up */
    curl_easy_cleanup(curl_handle);
    recv_buf_cleanup(&recv_buf);


    return (void *)(intptr_t)result;
}

int check_all_values_one(){

    int allValuesAreOne = 1;

    for (int i = 0; i < 49; i++) {
        if (image_strips_have[i] != 1) {
            allValuesAreOne = 0; 
            break; 
        }
    }

    return allValuesAreOne;
}

//  FOR CATPNG

    int convertBufToHostInt(U8* buf) {
    u_int32_t value;
    memcpy(&value, buf, sizeof(u_int32_t));
    return ntohl(value);
}
 
 int get_width_height_from_IHDR(char* filepath, int* width){

    FILE *fp = fopen(filepath, "rb");
    U8* buf = malloc(4); // first 4 bits is width, second 4 bits is height

    // get height & width
    fseek(fp, 8+4+4, SEEK_SET); // skip to width
    fread(buf, 4, 1, fp); // width

    *width = convertBufToHostInt(buf);

    fread(buf, 4, 1, fp); //height

    u_int32_t height = convertBufToHostInt(buf);

    fclose(fp);
    free(buf);

    return height;
}
 
 U8* get_IDAT_Data(char* filepath, int length){

    FILE *fp = fopen(filepath, "rb");
    U8* dataBuf = malloc(length);

    fseek(fp, 8+4+4+13+4+4+4, SEEK_SET); // skip to data
    fread(dataBuf, length, 1, fp); // read data
    fclose(fp);

    return dataBuf;
}

int get_IDAT_length(char* filepath){

    FILE *fp = fopen(filepath, "rb");
    U8* buf = malloc(4); 
    int length = 0;

    // get data length first
    fseek(fp, 8+4+4+13+4, SEEK_SET); // skip to data length 8+4+4
    fread(buf, 4, 1, fp); // data length
    length = convertBufToHostInt(buf);

    fclose(fp);
    free(buf);

    return length;
}

/*CATPNG FUNCTION*/
 void catpng(char** filePaths, int n){

    if ((n == 0) || (filePaths[0] == NULL)){
            printf("Error filePaths is empty\n");
            return;
        }    
        int heightSum = 0;
        int width = 0;
        int* heights = malloc(n*sizeof(int));

        // Get the height & Width

        for(int i = 0; i < n; i++){
            heights[i] = get_width_height_from_IHDR(filePaths[i], &width);
            heightSum += heights[i];
        }

        // Get the IDAT data & uncompress
        int length = 0;
        int totalLength = 0;
        int ret = 0;
        U64 len_inf = 0;      /* uncompressed data length                      */
        U8* bufTotal = NULL;
        size_t oldLength = 0;
        int length1 = 0;

        int number_of_channels = 4; // RGBA

        for(int i = 0; i < n; i++){
            length = get_IDAT_length(filePaths[i]);
            totalLength += length;

            if (i == 0){
                length1 = get_IDAT_length(filePaths[i]);
            }


            U8* buf = get_IDAT_Data(filePaths[i], length);

            // Need to use uncompressed_size to initlize gp_buf_inf
            int uncompressed_size = heights[i] * (width * number_of_channels + 1);
            U8* gp_buf_inf = malloc(uncompressed_size * sizeof(U8));

            // Uncompress buf using mem_inf:
            ret = mem_inf(gp_buf_inf, &len_inf, buf, length);
            if (ret == 0) { /* success */
               // printf("original len = %d, length = %lu, len_inf = %lu\n", \
                    uncompressed_size, length, len_inf);
            } else { /* failure */
               // fprintf(stderr,"mem_inf failed. ret = %d., len_inf = %d\n", ret, len_inf);
            }
            free(buf);

            // Create new buffer with length+oldLength and then copy contents

            size_t newLength = len_inf + oldLength;
            U8* newBuf = malloc(newLength);

            // Copy over old data to the new buffer
            if (oldLength != 0) {
            memcpy(newBuf, bufTotal, oldLength);
            free(bufTotal);
            }

            // Copy uncompressed data to bufTotal
            memcpy(newBuf + oldLength, gp_buf_inf, len_inf);
            free(gp_buf_inf);
            if (ret != 0) {
                free(gp_buf_inf);
                free(newBuf);
                free(heights);
                return;
            } else {
                bufTotal = newBuf;
            }

            oldLength = newLength;
        }

        // Recompress data in bufTotal
        U64 len_def = 0;  
        int bufferRoom = totalLength*0.2; // Extra wiggle room for totalLength (compression is inconsistent)
        totalLength = totalLength + bufferRoom;
        U8* gp_buf_def = malloc(totalLength);

        ret = mem_def(gp_buf_def, &len_def, bufTotal, oldLength, Z_DEFAULT_COMPRESSION);
        if (ret == 0) { /* success */
           // printf("original len = %d, len_def = %lu\n", oldLength, len_def);
        } else { /* failure */
           // fprintf(stderr,"mem_def failed. ret = %d.\n", ret);
        }

        // Create all.png with compressed data

        FILE *fp = fopen("all.png", "wb");

        u_int32_t widthNetworkOrder = htonl(width);

        // Copy over everything until IDAT chunk from first image

        FILE *fp2 = fopen(filePaths[0], "rb");
        int untilHeight = 8+4+4+4;
        U8* buf1 = malloc(untilHeight); // everything until Height
        fread(buf1, untilHeight, 1, fp2);
        fwrite(buf1 , 1, untilHeight, fp );
        free(buf1);

        u_int32_t heightSumNetworkOrder = htonl(heightSum);
        fwrite(&heightSumNetworkOrder, 1, 4, fp);

        U8* buf2 = malloc(5); // allocate for the rest of Data in IHDR
        fseek(fp2, 4, SEEK_CUR); // skip height
        fread(buf2, 5, 1, fp2);
        fwrite(buf2 , 1, 5, fp );

        // Calc crc IHDR
        U8 IHDRdata[17]; 
        memcpy(IHDRdata, "IHDR", 4); 
        memcpy(IHDRdata + 4, &widthNetworkOrder, 4);
        memcpy(IHDRdata + 8, &heightSumNetworkOrder, 4); 
        memcpy(IHDRdata + 12, buf2, 5); 
        free(buf2);

        u_int32_t IHDRcrc = htonl(crc(IHDRdata, 17));
        fwrite(&IHDRcrc, 1, 4, fp);

        fclose(fp2);

        // Need to calculate crc for IDAT for all.png

        char chunkType[4] = { 'I', 'D', 'A', 'T' };
        // Need to add type to IDAT Data:
        U8* IDATDataWithType = malloc(4 + totalLength);

        memcpy(IDATDataWithType, chunkType, 4);
        memcpy(IDATDataWithType + 4, gp_buf_def, totalLength);

        int crcAllPng = htonl(crc(IDATDataWithType, 4 + totalLength));
        free(IDATDataWithType);

        // IDAT

        u_int32_t totalLengthHost = htonl(totalLength);

        fwrite(&totalLengthHost, 1, 4, fp ); // length
        fwrite(chunkType, 1, 4, fp ); // Type
        fwrite(gp_buf_def, 1, totalLength, fp ); // write compressed data to all.png
        fwrite(&crcAllPng, 1, 4, fp ); // crc

        // IEND

        FILE *fp3 = fopen(filePaths[0], "rb");
        int IENDChunk = 8+4+4+13+4+4+4+length1+4;
        U8* buf3 = malloc(IENDChunk); 
        fseek(fp3, IENDChunk, SEEK_SET); // skip to IEND chunk
        fread(buf3, 4+4+0+4, 1, fp3); // read IEND chunk
        fwrite(buf3 , 1, 4+4+0+4, fp ); // write it to all.png
        free(buf3);
        fclose(fp3);

        // Free memory:
        fclose(fp);
        free(bufTotal);
        free(heights);
        free(gp_buf_def);

    }

int main(int argc, char** argv){
    int t,n;

    if(argv[1] == "t" && argv[6] == "n"){ //just check the order of the input
        t = argv[3];
        n = argv[8];
        
    }
    else if(argv[1] == "n" && argv[6] == "t"){
        n = argv[3];
        t = argv[8];

    }
    else {
        t = argv[3];
        n = argv[8];
    }
    char url[256];
    // Need to add arg inputs

    // The url needs to be an arg input (i.e change img=1 to 2 or 3 depending on input)
    
    char* urls[] = {
    // "http://ece252-1.uwaterloo.ca:2520/image?img=2",
    // "http://ece252-2.uwaterloo.ca:2520/image?img=2",
    // "http://ece252-3.uwaterloo.ca:2520/image?img=2"
     
     sprintf(url, "%s%d", "http://ece252-1.uwaterloo.ca:2520/image?img=", n), 
     sprintf(url, "%s%d", "http://ece252-2.uwaterloo.ca:2520/image?img=", n), 
     sprintf(url, "%s%d", "http://ece252-3.uwaterloo.ca:2520/image?img=", n)

    };

    const char *folder = "image_strips";
    char delete_folder[256];
    snprintf(delete_folder, sizeof(delete_folder), "rm -r %s", folder);

    int NUM_THREADS = t; //10;  NUM_THREADS needs to be an arg input

    struct thread_args in_params[NUM_THREADS];
    int p_results[NUM_THREADS];
    pthread_t *p_tids = malloc(sizeof(pthread_t) * NUM_THREADS);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Create x number of threads and wait for them,
    // if we don't have all image strips -> repeat
    while (check_all_values_one() == 0)
    {
    for (int i=0; i<NUM_THREADS; i++) {
        // Alternate requests between the 3 servers
        strcpy(in_params[i].url, urls[i % 3]);

        printf("Creating thread with PID: %lu and url: %s\n", p_tids + i, in_params[i].url);

        int create_result = pthread_create(p_tids + i, NULL, get_image_strip, in_params + i);
        if (create_result != 0) {
        fprintf(stderr, "Error creating thread: %d\n", create_result);
    }
    }

    for (int i=0; i<NUM_THREADS; i++) {
        pthread_join(p_tids[i], (void **)&(p_results[i]));
        printf("thread terminated with result: %d\n", p_results[i]);
        if (p_results[i] >= 0){
            image_strips_have[p_results[i]] = 1;
        }
    }
    }

    free(p_tids);
    curl_global_cleanup();


    // NEED TO call catpng here:
    //0 is top images and 49 is bottom 

    DIR *dir;
    struct dirent *ent;
    char **filepaths;
    int s = 0;
    if((dir = opendir (folder)) != NULL) {
    /* store all files within directory to filepaths */
        while ((ent = readdir (dir)) != NULL) {
            realpath(ent->d_name, filepaths[s]);
            s++;
        }

  }
  closedir(dir);

    catpng(filepaths, 49); 


    
    // delete image_strips folder
   system(delete_folder);

    return 0;
}
