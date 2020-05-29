/*
 * Copyright 2020 Leo McCormack
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * @file saf_reverb_internal.h
 * @brief Internal part of the reverb processing module (saf_reverb)
 *
 * A collection of reverb and room simulation algorithms.
 *
 * @author Leo McCormack
 * @date 06.05.2020
 */

#ifndef __REVERB_INTERNAL_H_INCLUDED__
#define __REVERB_INTERNAL_H_INCLUDED__

#include <stdio.h>
#include <math.h> 
#include <string.h>
#include <assert.h>
#include "saf_reverb.h"
#include "saf_utilities.h"
#include "saf_sh.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* ========================================================================== */
/*                         IMS Shoebox Room Simulator                         */
/* ========================================================================== */

/** Number of wall for a shoebox room */
#define IMS_NUM_WALLS_SHOEBOX ( 6 )
/** FIR filter order (must be even) */
#define IMS_FIR_FILTERBANK_ORDER ( 400 )
/** IIR filter order (1st or 3rd) */
#define IMS_IIR_FILTERBANK_ORDER ( 3 )
/** Circular buffer length */
#define IMS_CIRC_BUFFER_LENGTH ( 2*8192U )
/** Circular buffer length, minus 1 */
#define IMS_CIRC_BUFFER_LENGTH_MASK ( IMS_CIRC_BUFFER_LENGTH - 1U )
/** Maximum number of samples that ims should expect to process at a time */
#define IMS_MAX_NSAMPLES_PER_FRAME ( 20000 )

/**
 * Void pointer (improves readability when working with arrays of handles)
 */
typedef void* voidPtr;

/**
 * Union struct for Cartesian coordinates (access as .x,.y,.z, or .v[3])
 */
typedef struct _ims_pos_xyz {
    union {
        struct { float x, y, z; };
        float v[3];
    };
} ims_pos_xyz;

/**
 * Supported receiver types
 */
typedef enum _RECEIVER_TYPES{
    RECEIVER_SH   /**< Spherical harmonic receiver */
}RECEIVER_TYPES;

/**
 * Source object
 */
typedef struct _ims_src_obj{
    float* sig;      /**< Source signal pointer */
    ims_pos_xyz pos; /**< Source position */
    int ID;          /**< Unique Source ID */
} ims_src_obj;

/**
 * Receiver object
 */
typedef struct _ims_rec_obj{
    float** sigs;        /**< Receiver signal pointers (one per channel) */
    RECEIVER_TYPES type; /**< Receiver type (see RECEIVER_TYPES enum) */
    int nChannels;       /**< Number of channels for receiver */
    ims_pos_xyz pos;     /**< Source position */
    int ID;              /**< Unique Source ID */
} ims_rec_obj;

/**
 * Echogram structure
 */
typedef struct _echogram_data
{
    int numImageSources;  /**< Number of image sources in echogram */
    int nChannels;        /**< Number of channels */
    float** value;        /**< Echogram magnitudes per image source and channel;
                           *   numImageSources x nChannels */
    float* time;          /**< Propagation time (in seconds) for each image
                           *   source; numImageSources x 1 */
    int** order;          /**< Reflection order for each image and dimension;
                           *   numImageSources x 3 */
    ims_pos_xyz* coords;  /**< Reflection coordinates (Cartesian);
                           *   numImageSources x 3 */
    int* sortedIdx;       /**< Indices that sort the echogram based on
                           *   propagation time, in accending order;
                           *   numImageSources x 1 */

} echogram_data;

/**
 * Helper structure, comprising variables used when computing echograms and
 * rendering RIRs. The idea is that there should be one instance of this per
 * source/reciever combination.
 */
typedef struct _ims_core_workspace
{
    /* Locals */
    int room[3];          /**< Room dimensions, in meters */
    float d_max;          /**< Maximum distance, in meters */
    ims_pos_xyz src;      /**< Source position */
    ims_pos_xyz rec;      /**< Receiver position */
    int nBands;           /**< Number of bands */

    /* Internal */
    int Nx, Ny, Nz;
    int lengthVec, numImageSources;
    int* validIDs;
    float* II, *JJ, *KK;
    float* s_x, *s_y, *s_z, *s_d, *s_t, *s_att;

    /* Echograms */
    int refreshEchogramFLAG;
    void* hEchogram;
    void* hEchogram_rec;
    voidPtr* hEchogram_abs;

    /* Room impulse responses (only used/allocated when a render function is
     * called) */
    int refreshRIRFLAG;
    int rir_len_samples;
    float rir_len_seconds;
    float*** rir_bands; /* nBands x nChannels x rir_len_samples */
 
}ims_core_workspace;

/**
 * Main structure for IMS. It comprises variables describing the room, and the
 * source and receiver objects within it. It also includes "core workspace"
 * handles for each source/receiver combination.
 */
typedef struct _ims_scene_data
{
    /* Locals */
    int room_dimensions[3];  /**< Room dimensions, in meters */
    float c_ms;              /**< Speed of sound, in ms^1 */
    float fs;                /**< Sampling rate */
    int nBands;              /**< Number of frequency bands */
    float** abs_wall;        /**< Wall aborption coeffs per wall; nBands x 6 */

    /* Source and receiver positions */
    ims_src_obj srcs[IMS_MAX_NUM_SOURCES];   /**< Source positions*/
    ims_rec_obj recs[IMS_MAX_NUM_RECEIVERS]; /**< Receiver positions*/
    long nSources;           /**< Current number of sources */
    long nReceivers;         /**< Current number of receivers */

    /* Internal */
    voidPtr** hCoreWrkSpc;   /**< One per source/receiver combination */
    float* band_centerfreqs; /**< Octave band CENTRE frequencies; nBands x 1 */
    float* band_cutofffreqs; /**< Octave band CUTOFF frequencies;
                              *   (nBands-1) x 1 */
    float** H_filt;          /**< nBands x (IMS_FIR_FILTERBANK_ORDER+1) */
    ims_rir** rirs;          /**< One per source/receiver combination */

    /* Circular buffers (only used/allocated when "applyEchogramTD" function is
     * called for the first time) */
    unsigned int wIdx;
    float*** circ_buffer;    /**< nChannels x nBands x IMS_CIRC_BUFFER_LENGTH */

    /* IIR filterbank (only used/allocated when "applyEchogramTD" function is
     * called for the first time) */
    voidPtr* hFaFbank;       /**< One per source */
    float*** src_sigs_bands; /**< nSources x nBands x nSamples */

} ims_scene_data;


/* =========================== Internal Functions =========================== */

/**
 * Creates an instance of the core workspace
 *
 * The idea is that there is one core workspace instance per source/receiver
 * combination.
 *
 * @param[in] phWork (&) address of the workspace handle
 * @param[in] nBands Number of bands
 */
void ims_shoebox_coreWorkspaceCreate(void** phWork,
                                     int nBands);

/**
 * Destroys an instance of the core workspace
 *
 * @param[in] phWork  (&) address of the workspace handle
 */
void ims_shoebox_coreWorkspaceDestroy(void** phWork);

/**
 * Creates an instance of an echogram container
 *
 * @param[in] phEcho (&) address of the echogram container
 */
void ims_shoebox_echogramCreate(void** phEcho);

/**
 * Resizes an echogram container
 *
 * @note The container is only resized if the number of image sources or
 *       channels have changed.
 *
 * @param[in] hEcho           echogram container
 * @param[in] numImageSources New number of image sources
 * @param[in] nChannels       New number of channels
 */
void ims_shoebox_echogramResize(void* hEcho,
                                int numImageSources,
                                int nChannels);

/**
 * Destroys an instance of an echogram container
 *
 * @param[in] phEcho (&) address of the echogram container
 */
void ims_shoebox_echogramDestroy(void** phEcho);

/**
 * Calculates an echogram of a rectangular space using the image source method,
 * for a specific source/reciever combination
 *
 * Note the coordinates of source/receiver are specified from the left ground
 * corner of the room:
 *
 * \verbatim
 *
 *                ^x
 *             __|__    _
 *             |  |  |   |
 *             |  |  |   |
 *          y<----.  |   | l
 *             |     |   |
 *             |     |   |
 *             o_____|   -
 *
 *             |-----|
 *                w
 *
 * \endverbatim
 *
 * @param[in] hWork   workspace handle
 * @param[in] room    Room dimensions, in meters
 * @param[in] src     Source position, in meters
 * @param[in] rec     Receiver position, in meters
 * @param[in] maxTime Maximum propagation time to compute the echogram, seconds
 * @param[in] c_ms    Speed of source, in meters per second
 */
void ims_shoebox_coreInit(void* hWork,
                          int room[3],
                          ims_pos_xyz src,
                          ims_pos_xyz rec,
                          float maxTime,
                          float c_ms);

/**
 * Imposes spherical harmonic directivies onto the echogram computed with
 * ims_shoebox_coreInit, for a specific source/reciever combination
 *
 * @note Call ims_shoebox_coreInit before applying the directivities
 *
 * @param[in] hWork    workspace handle
 * @param[in] sh_order Spherical harmonic order
 */
void ims_shoebox_coreRecModuleSH(void* hWork,
                                 int sh_order);

/**
 * Applies boundary absoption per frequency band, onto the echogram computed
 * with ims_shoebox_coreRecModuleSH, for a specific source/reciever combination
 *
 * Absorption coefficients are given for each of the walls on the respective
 * planes [x+ y+ z+; x- y- z-].
 *
 * @note Call ims_shoebox_coreRecModuleX before applying the absoption
 *
 * @param[in] hWork    workspace handle
 * @param[in] abs_wall Absorption coefficients; nBands x 6
 */
void ims_shoebox_coreAbsorptionModule(void* hWork,
                                      float** abs_wall);

/**
 * Renders a room impulse response for a specific source/reciever combination
 *
 * @note Call ims_shoebox_coreAbsorptionModule before rendering rir
 *
 * @param[in]  hWork               workspace handle
 * @param[in]  fractionalDelayFLAG 0: disabled, 1: use Lagrange interpolation
 * @param[in]  fs                  SampleRate, Hz
 * @param[in]  H_filt              filterbank; nBands x (filterOrder+1)
 * @param[out] rir                 Room impulse response
 */
void ims_shoebox_renderRIR(void* hWork,
                           int fractionalDelayFLAG,
                           float fs,
                           float** H_filt,
                           ims_rir* rir);


#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* __REVERB_INTERNAL_H_INCLUDED__ */