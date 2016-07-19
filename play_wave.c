/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Code from rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 *
 */

#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#include <unistd.h>
#include "mirsdrapi-rsp.h"

#define DEFAULT_SAMPLE_RATE             2048000
#define DEFAULT_BUF_LENGTH              (336 * 2) // (16 * 16384)
#define MINIMAL_BUF_LENGTH              672 // 512
#define MAXIMAL_BUF_LENGTH              (256 * 16384)

static int do_exit = 0;
static uint64_t bytes_to_read = 0;

short *ibuf;
short *qbuf;
unsigned int firstSample;
int samplesPerPacket, grChanged, fsChanged, rfChanged;

double atofs(char *s)
/* standard suffixes */
{
    char last;
    int len;
    double suff = 1.0;
    len = strlen(s);
    last = s[len-1];
    s[len-1] = '\0';
    switch (last) {
    case 'g':
    case 'G':
        suff *= 1e3;
    case 'm':
    case 'M':
        suff *= 1e3;
    case 'k':
    case 'K':
        suff *= 1e3;
        suff *= atof(s);
        s[len-1] = last;
        return suff;
    }
    s[len-1] = last;
    return atof(s);
}

void usage(void)
{
    fprintf(stderr,
            "play_sdr, an I/Q recorder for SDRplay RSP receivers\n\n"
            "Usage:\t -f frequency_to_tune_to [Hz]\n"
            "\t[-s samplerate (default: 2048000 Hz)]\n"
            "\t[-g gain (default: 50)]\n"
            "\t[-n number of samples to read (default: 0, infinite)]\n"
            "\t[-R enable gain reduction (default: 0, disabled)]\n"
            "\t[-L RSP LNA enable (default: 0, disabled)]\n"
            "\tfilename (a '-' dumps samples to stdout)\n\n");
    exit(1);
}


//////////////////////////////////////

// datetime

typedef struct {
    uint16_t year;
    uint16_t month;
    uint16_t day_of_week;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint16_t milliseconds;
} __attribute__((packed)) datetime_t;

// riff

typedef struct {
    char id[4];
    uint32_t size;
    char type[4];
} __attribute__((packed)) riff_t;

// fmt

typedef struct {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t samples_per_sec;
    uint32_t data_rate;
    uint16_t block_size;
    uint16_t bits_per_sample;
} __attribute__((packed)) fmt_t;

// auxi

typedef struct {
    datetime_t start_time;
    datetime_t stop_time;
    uint32_t frequency; //receiver center frequency
    uint32_t sample_frequency; //A/D sample frequency before downsampling
    uint32_t if_frequency; //IF freq if an external down converter is used
    uint32_t bandwidth; //displayable BW
    uint32_t dc_offset; //DC offset of I/Q channels in 1/1000's of a count
} __attribute__((packed)) auxi_t;

// chunk

typedef struct {
    char id[4];
    uint32_t size;
} __attribute__((packed)) chunk_t;


void set_datetime(datetime_t* dt)
{
    time_t rawtime = time(NULL);
    struct tm *tm = gmtime(&rawtime);
    dt->year = tm->tm_year + 1900;
    dt->month = tm->tm_mon + 1;
    dt->day = tm->tm_mday;
    dt->hour = tm->tm_hour;
    dt->minute = tm->tm_min;
    dt->second = tm->tm_sec;
}

void wave_header(FILE *file, uint32_t samp_rate, uint32_t frequency, uint32_t bits_per_sample)
{
    riff_t riff;
    fmt_t fmt;
    chunk_t chunk;
    auxi_t auxi;

    // write riff header
    memset(&riff, 0, sizeof(riff_t));
    strncpy(riff.id, "RIFF", 4);
    strncpy(riff.type, "WAVE", 4);
    riff.size = -1;
    if (fwrite(&riff, 1, sizeof(riff_t), file) != sizeof(riff_t)) exit(1);

    // write fmt header
    memset(&chunk, 0, sizeof(chunk_t));
    strncpy(chunk.id, "fmt ", 4);
    chunk.size = sizeof(fmt_t);
    if (fwrite(&chunk, 1, sizeof(chunk_t), file) != sizeof(chunk_t)) exit(1);

    // write fmt data
    memset(&fmt, 0, sizeof(fmt_t));
    fmt.format_tag = 1; // PCM
    fmt.channels = 2;
    fmt.bits_per_sample = bits_per_sample;
    fmt.samples_per_sec = samp_rate;
    fmt.data_rate = fmt.channels * fmt.bits_per_sample / 8 * fmt.samples_per_sec;
    fmt.block_size = fmt.channels * fmt.bits_per_sample / 8;
    if (fwrite(&fmt, 1, sizeof(fmt_t), file) != sizeof(fmt_t)) exit(1);

    // write auxi header
    memset(&chunk, 0, sizeof(chunk_t));
    strncpy(chunk.id, "auxi", 4);
    chunk.size = sizeof(auxi_t);
    if (fwrite(&chunk, 1, sizeof(chunk_t), file) != sizeof(chunk_t)) exit(1);

    // write auxi data
    memset(&auxi, 0, sizeof(auxi_t));
    auxi.frequency = frequency;
    set_datetime(&auxi.start_time);
    if (fwrite(&auxi, 1, sizeof(auxi_t), file) != sizeof(auxi_t)) exit(1);

    // write data header
    strncpy(chunk.id, "data", 4);
    chunk.size = -1;
    if (fwrite(&chunk, 1, sizeof(chunk_t), file) != sizeof(chunk_t)) exit(1);
}

float db(float x)
{
    return 10 * logf(x);    
}

int interval_seconds = 2; 

//////////////////////////////////////


int main(int argc, char **argv)
{
    char *filename = NULL;
    int n_read;
    mir_sdr_ErrT r;
    int opt;
    int gain = 50;
    FILE *file;
    short *buffer;
    uint32_t frequency = 100000000;
    uint32_t samp_rate = DEFAULT_SAMPLE_RATE;
    uint32_t out_block_size = DEFAULT_BUF_LENGTH;
    int rspMode = 0;
    int rspLNA = 0;
    int i, j;

    while ((opt = getopt(argc, argv, "f:g:s:n:r:l")) != -1) {
        switch (opt) {
        case 'f':
            frequency = (uint32_t)atofs(optarg);
            break;
        case 'g':
            gain = (int)atof(optarg);
            break;
        case 's':
            samp_rate = (uint32_t)atofs(optarg);
            break;
        case 'n':
            bytes_to_read = (uint32_t)atofs(optarg) * 2;
            break;
        case 'r':
            rspMode = atoi(optarg);
            break;
        case 'l':
            rspLNA = atoi(optarg);
            break;
        default:
            usage();
            break;
        }
    }
    
    if (argc <= optind) {
        usage();
    } else {
        filename = argv[optind];
    }
    
    if(out_block_size < MINIMAL_BUF_LENGTH ||
       out_block_size > MAXIMAL_BUF_LENGTH ){
        fprintf(stderr,
                "Output block size wrong value, falling back to default\n");
        fprintf(stderr,
                "Minimal length: %u\n", MINIMAL_BUF_LENGTH);
        fprintf(stderr,
                "Maximal length: %u\n", MAXIMAL_BUF_LENGTH);
        out_block_size = DEFAULT_BUF_LENGTH;
    }
    
    buffer = malloc(out_block_size * sizeof(short));
   
    // gain reduction in db = 40
    // sample frequency in mhz = 2.0
    // frequency in mhz = 100.0
    // bandwidth to use = [1.536 mhz]
    // if to use = [IF_zero]
    // fills in with the number of samples returned by readpacket

    r = mir_sdr_Init(40, 2.0, 100.00, mir_sdr_BW_1_536, mir_sdr_IF_Zero,
                     &samplesPerPacket);
    if (r != mir_sdr_Success) {
        fprintf(stderr, "Failed to open SDRplay RSP device.\n");
        exit(1);
    }

    // uninitialize the API

    mir_sdr_Uninit();
    
    if(strcmp(filename, "-") == 0) { /* Write samples to stdout */
        file = stdout;
    } else {
        file = fopen(filename, "wb");
        if (!file) {
            fprintf(stderr, "Failed to open %s\n", filename);
            goto out;
        }
    }
   

    //////////////////////////////////////////

    int interval = 0;
    float ipeak = 0, qpeak = 0;
    float iavg = 0, qavg = 0;

    wave_header(file, samp_rate, frequency, 16);

    //////////////////////////////////////////


    if (rspMode == 1){
        mir_sdr_SetParam(201,1);
        if (rspLNA == 1){
            mir_sdr_SetParam(202,0);
        }
        else {
            mir_sdr_SetParam(202,1);
        }
        r = mir_sdr_Init(gain, (samp_rate/1e6), (frequency/1e6),
                         mir_sdr_BW_1_536, mir_sdr_IF_Zero, &samplesPerPacket );
    } else {

        // gain reduction
        // sample rate in mhz
        // center frequency 
        // bandwidth = 1.536 mhz
        // if to use = if_zero

        r = mir_sdr_Init((78-gain), (samp_rate/1e6), (frequency/1e6),
                         mir_sdr_BW_1_536, mir_sdr_IF_Zero, &samplesPerPacket );
    }
    
    if (r != mir_sdr_Success) {
        fprintf(stderr, "Failed to start SDRplay RSP device.\n");
        exit(1);
    }
   
    // dc offset correction mode = one shot (4)
    // spped up = disabled (0) 

    mir_sdr_SetDcMode(4,0);

    // time period dc is tracked in one shot mode = 3 * 63 usec

    mir_sdr_SetDcTrackTime(63);
    
    ibuf = malloc(samplesPerPacket * sizeof(short));
    qbuf = malloc(samplesPerPacket * sizeof(short));
    
    fprintf(stderr, "Writing samples...\n");
    while (!do_exit) {

        // isamples
        // qsamples
        // location of first sample? (not used)
        // gain reduction changed? (not used)
        // rf frequency changed? (not used)
        // sample frequency changed? (not used)

        r = mir_sdr_ReadPacket(ibuf, qbuf, &firstSample, &grChanged, &rfChanged,
                               &fsChanged);
        if (r != mir_sdr_Success) {
            fprintf(stderr, "WARNING: ReadPacket failed.\n");
            break;
        }
        
        j = 0;
        for (i=0; i < samplesPerPacket; i++)
        {
            buffer[j++] = ibuf[i];
            buffer[j++] = qbuf[i];
        }

        //////////////////////////////////////////

        for (i=0; i < samplesPerPacket; i++)
        {
            float ival = (float) ibuf[i] / 32768;
            float qval = (float) qbuf[i] / 32768;
            ival = ival * ival;
            qval = qval * qval;
            iavg += ival;
            qavg += qval;
            if (ival > ipeak) ipeak = ival;
            if (qval > qpeak) qpeak = qval;
        }

        interval += samplesPerPacket;
        if (interval > samp_rate * interval_seconds){
            iavg /= interval;
            qavg /= interval;
            fprintf(stderr, "PEAK %5.1f | %5.1f dBFS   PAR %4.1f | %4.1f dB\n",
                    db(ipeak), db(qpeak), db(ipeak / iavg), db(qpeak / qavg));
            interval = 0;
            ipeak = qpeak = iavg = qavg = 0; 
        };

        //////////////////////////////////////////

        n_read = (samplesPerPacket * 2);
        
        if ((bytes_to_read > 0) && (bytes_to_read <= (uint32_t) n_read)) {
            n_read = bytes_to_read;
            do_exit = 1;
        }
        
        if (fwrite(buffer, 2, n_read, file) != (size_t)n_read) {
            fprintf(stderr, "Short write, samples lost, exiting!\n"); break;
        }
        
        if ((uint32_t)n_read < out_block_size) {
            fprintf(stderr, "Short read, samples lost, exiting!\n");
            break;
        }
        
        if (bytes_to_read > 0)
            bytes_to_read -= n_read;
    }
    
    if (do_exit)
        fprintf(stderr, "\nUser cancel, exiting...\n");
    else
        fprintf(stderr, "\nLibrary error %d, exiting...\n", r);
    
    if (file != stdout)
        fclose(file);
    
    mir_sdr_Uninit();
    free (buffer);
 out:
    return r >= 0 ? r : -r;
}

