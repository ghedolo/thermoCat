#pragma once
#include <cmath>

// Frequency response compensation table for speaker C2_F3a
// 25 bands, 1/3 octave, 62 Hz - 16 kHz
// compensation_db: amount to add to flatten response

struct FreqBand {
    float freq;
    float compensation_db;
};

static const FreqBand COMP_TABLE[] = {
    {62,    12.0},
    {79,    12.0},
    {99,    12.0},
    {125,   12.0},
    {157,   12.0},
    {198,   12.0},
    {250,   12.0},
    {315,   11.92},
    {397,   10.62},
    {500,   6.26},
    {630,   4.6},
    {794,   0.49},
    {1000,  0.0},
    {1260,  -2.1},
    {1587,  -0.95},
    {2000,  -4.02},
    {2520,  -4.29},
    {3175,  -6.4},
    {4000,  -8.73},
    {5040,  -6.43},
    {6350,  -4.31},
    {8000,  -2.71},
    {10079, 0.1},
    {12699, 4.75},
    {16000, 9.13},
};

static const int COMP_TABLE_SIZE = sizeof(COMP_TABLE) / sizeof(COMP_TABLE[0]);

// Interpolate compensation in dB for a given frequency
inline float getCompensationDb(float freq) {
    if (freq <= COMP_TABLE[0].freq) return COMP_TABLE[0].compensation_db;
    if (freq >= COMP_TABLE[COMP_TABLE_SIZE - 1].freq)
        return COMP_TABLE[COMP_TABLE_SIZE - 1].compensation_db;

    for (int i = 0; i < COMP_TABLE_SIZE - 1; i++) {
        if (freq >= COMP_TABLE[i].freq && freq < COMP_TABLE[i + 1].freq) {
            float t = (freq - COMP_TABLE[i].freq) /
                      (COMP_TABLE[i + 1].freq - COMP_TABLE[i].freq);
            return COMP_TABLE[i].compensation_db * (1.0f - t) +
                   COMP_TABLE[i + 1].compensation_db * t;
        }
    }
    return 0.0f;
}

// Convert dB to linear gain
inline float dbToGain(float db) {
    return powf(10.0f, db / 20.0f);
}

// Get linear gain multiplier for a given frequency
inline float getCompensationGain(float freq) {
    return dbToGain(getCompensationDb(freq));
}
