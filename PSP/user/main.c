#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspiofilemgr.h>

#include <math.h>
#include <string.h>

#include "kubridge.h"

PSP_MODULE_INFO("PSP_MIDI_SYNTH", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

// CONFIG
#define SAMPLE_RATE   44100
#define BUF_SAMPLES   512
#define MAX_VOICES    4
#define VOLUME        PSP_AUDIO_VOLUME_MAX
#define TWO_PI        6.28318530718f

#define GAIN_SINE      2.1f
#define GAIN_TRIANGLE  2.4f
#define GAIN_SAW       1.2f
#define GAIN_SQUARE    1.0f

#define PRESET_COUNT   5
#define PRESET_FILE "ms0:/PSP/GAME/PSP_MidiSynth/presets.bin"

// kernel.prx

void pspUARTInit(int baud);
int  pspUARTRead(void);


static volatile int done = 0;

int exit_callback(int a1, int a2, void *c) {
    done = 1;
    return 0;
}

int CallbackThread(SceSize a, void *b) {
    int cb = sceKernelCreateCallback("Exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

void SetupCallbacks(void) {
    int th = sceKernelCreateThread("CB", CallbackThread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
}

// kernel.prx loader

static void LoadKernel(const char *path) {
    int st;
    SceUID m = kuKernelLoadModule(path, 0, NULL);
    if (m >= 0)
        sceKernelStartModule(m, 0, NULL, &st, NULL);
}


typedef enum {
    WAVE_SINE = 0,
    WAVE_SQUARE,
    WAVE_TRIANGLE,
    WAVE_SAW,
    WAVE_COUNT
} Waveform;

static const char *waveNames[WAVE_COUNT] = {
    "SINE", "SQUARE", "TRIANGLE", "SAW"
};

typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
} EnvState;

typedef struct {
    EnvState state;
    float level;
} ADSR;

typedef struct {
    int   active;
    int   note;
    float freq;
    float phase;
    float filter;
    ADSR  env;
} Voice;

// Presets

typedef struct {
    float cutoff;
    float res;
    float attack;
    float decay;
    float sustain;
    float release;
    int   wave;
} Preset;

static Preset g_presets[PRESET_COUNT] = {
    {0.40f, 0.20f, 0.05f, 0.20f, 0.70f, 0.30f, WAVE_SQUARE},   // preset1
    {0.50f, 0.15f, 0.08f, 0.25f, 0.60f, 0.40f, WAVE_SINE},     // preset2
    {0.30f, 0.30f, 0.10f, 0.30f, 0.50f, 0.25f, WAVE_SAW},      // preset3
    {0.70f, 0.10f, 0.03f, 0.15f, 0.80f, 0.60f, WAVE_TRIANGLE}, // preset4
    {0.25f, 0.25f, 0.15f, 0.40f, 0.40f, 0.20f, WAVE_SQUARE}    // preset5
};

static int g_currentPreset = 0;

// Globals
static Voice g_voices[MAX_VOICES];
static volatile int g_voiceIndex = 0;
static volatile Waveform g_wave = WAVE_SQUARE;
static int g_audioCh = -1;

// Pitch bend: -1.0 / +1.0
static volatile float g_pitchBend = 0.0f;

// ADSR params
static float g_attack  = 0.05f;
static float g_decay   = 0.20f;
static float g_sustain = 0.70f;
static float g_release = 0.30f;

// Filter params
static float g_cutoff = 0.4f;
static float g_res    = 0.2f;


static float clampf(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

static float midiNoteToFreq(int n) {
    return 440.0f * powf(2.0f, (n - 69) / 12.0f);
}

static int SavePresets(void) {
    SceUID fd = sceIoOpen(PRESET_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;

    int size = sizeof(g_presets);
    int written = sceIoWrite(fd, g_presets, size);
    sceIoClose(fd);

    return (written == size) ? 0 : -1;
}

static int LoadPresets(void) {
    SceUID fd = sceIoOpen(PRESET_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return -1;

    int size = sizeof(g_presets);
    int read = sceIoRead(fd, g_presets, size);
    sceIoClose(fd);

    if (read != size) return -1;
    return 0;
}

static void applyPreset(int idx) {
    if (idx < 0 || idx >= PRESET_COUNT) return;

    Preset *p = &g_presets[idx];
    g_cutoff  = clampf(p->cutoff,  0.0f, 1.0f);
    g_res     = clampf(p->res,     0.0f, 1.0f);
    g_attack  = clampf(p->attack,  0.0f, 1.0f);
    g_decay   = clampf(p->decay,   0.0f, 1.0f);
    g_sustain = clampf(p->sustain, 0.0f, 1.0f);
    g_release = clampf(p->release, 0.0f, 1.0f);

    if (p->wave < 0) p->wave = 0;
    if (p->wave >= WAVE_COUNT) p->wave = WAVE_COUNT - 1;
    g_wave = (Waveform)p->wave;
}

//MIDI Thread
static int MidiThread(SceSize a, void *b) {
    int rs = 0, d1 = -1;

    while (!done) {
        int v = pspUARTRead();
        if (v < 0) {
            sceKernelDelayThread(100);
            continue;
        }

        unsigned char c = (unsigned char)v;
        if (c >= 0xF8) continue;

        if (c & 0x80) {
            rs = c;
            d1 = -1;
            continue;
        }

        if (!(rs & 0x80)) continue;

        unsigned char status = rs & 0xF0;
        unsigned char chan   = rs & 0x0F;
        if (chan != 0) continue;

        // Pitch Bend
        if (status == 0xE0) {
            if (d1 < 0) {
                d1 = c;
            } else {
                int lsb = d1;
                int msb = c;
                d1 = -1;

                int value = (msb << 7) | lsb;   /* 0..16383 */
                g_pitchBend = (value - 8192) / 8192.0f;

                scePowerTick(PSP_POWER_TICK_DISPLAY);
            }
            continue;
        }

        // Note On / Off
        if (status == 0x90 || status == 0x80) {
            if (d1 < 0) {
                d1 = c;
            } else {
                int note = d1;
                int vel  = c;
                d1 = -1;

                scePowerTick(PSP_POWER_TICK_DISPLAY);

                if (status == 0x90 && vel > 0) {
                    int v = g_voiceIndex;
                    g_voiceIndex = (g_voiceIndex + 1) % MAX_VOICES;

                    g_voices[v].active = 1;
                    g_voices[v].note = note;
                    g_voices[v].freq = midiNoteToFreq(note);
                    g_voices[v].phase = 0.0f;
                    g_voices[v].filter = 0.0f;
                    g_voices[v].env.state = ENV_ATTACK;
                    g_voices[v].env.level = 0.0f;
                } else {
                    for (int i = 0; i < MAX_VOICES; i++) {
                        if (g_voices[i].active && g_voices[i].note == note)
                            g_voices[i].env.state = ENV_RELEASE;
                    }
                }
            }
        }
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

// Audio Thread
static int AudioThread(SceSize a, void *b) {
    static short buf[BUF_SAMPLES * 2];
    Voice vloc[MAX_VOICES];

    while (!done) {
        memcpy(vloc, g_voices, sizeof(vloc));
        float pitch = g_pitchBend;

        /* pitch bend range: +-2 semitones */
        float pitchMul = powf(2.0f, (pitch * 2.0f) / 12.0f);

        for (int i = 0; i < BUF_SAMPLES; i++) {
            float mix = 0.0f;

            for (int v = 0; v < MAX_VOICES; v++) {
                if (!vloc[v].active) continue;

                float p = vloc[v].phase;
                float s = 0.0f, g = 1.0f;

                switch (g_wave) {
                    case WAVE_SINE:     s = sinf(p * TWO_PI); g = GAIN_SINE; break;
                    case WAVE_SQUARE:   s = (p < 0.5f) ? 1.f : -1.f; g = GAIN_SQUARE; break;
                    case WAVE_TRIANGLE: s = (p < 0.5f) ? (-1 + 4*p) : (3 - 4*p); g = GAIN_TRIANGLE; break;
                    case WAVE_SAW:      s = 2*p - 1; g = GAIN_SAW; break;
                    default:            s = 0; break;
                }

                ADSR *e = &vloc[v].env;
                switch (e->state) {
                    case ENV_ATTACK:
                        e->level += 1.0f / (clampf(g_attack, 0.001f, 1.0f) * SAMPLE_RATE);
                        if (e->level >= 1.0f) { e->level = 1.0f; e->state = ENV_DECAY; }
                        break;
                    case ENV_DECAY:
                        e->level -= (1.0f - g_sustain) / (clampf(g_decay, 0.001f, 1.0f) * SAMPLE_RATE);
                        if (e->level <= g_sustain) { e->level = g_sustain; e->state = ENV_SUSTAIN; }
                        break;
                    case ENV_RELEASE:
                        e->level -= g_sustain / (clampf(g_release, 0.001f, 1.0f) * SAMPLE_RATE);
                        if (e->level <= 0.0f) {
                            e->level = 0.0f;
                            e->state = ENV_IDLE;
                            vloc[v].active = 0;
                        }
                        break;
                    default:
                        break;
                }

                s *= e->level * g;

                float z = vloc[v].filter;
                z += clampf(g_cutoff, 0.01f, 1.0f) * (s - z);
                z -= clampf(g_res, 0.0f, 0.95f) * z;
                vloc[v].filter = z;

                mix += z;

                float freq = vloc[v].freq * pitchMul;
                p += freq / SAMPLE_RATE;
                if (p >= 1.0f) p -= 1.0f;
                vloc[v].phase = p;
            }

            short out = (short)(mix * 20000 * 0.25f);
            buf[i*2] = buf[i*2+1] = out;
        }

        memcpy(g_voices, vloc, sizeof(vloc));
        sceAudioOutputPannedBlocking(g_audioCh, VOLUME, VOLUME, buf);
    }

    sceKernelExitDeleteThread(0);
    return 0;
}



int main(int argc, char *argv[]) {
    pspDebugScreenInit();
    SetupCallbacks();
    LoadKernel("kernel.prx");
    pspUARTInit(31250);

    if (LoadPresets() != 0) {
        SavePresets();
    }
    g_currentPreset = 0;
    applyPreset(g_currentPreset);

    g_audioCh = sceAudioChReserve(
        PSP_AUDIO_NEXT_CHANNEL,
        BUF_SAMPLES,
        PSP_AUDIO_FORMAT_STEREO
    );

    SceUID midiTh  = sceKernelCreateThread("MIDI",  MidiThread,  0x18, 0x2000, 0, NULL);
    SceUID audioTh = sceKernelCreateThread("AUDIO", AudioThread, 0x14, 0x2000, 0, NULL);

    sceKernelStartThread(midiTh, 0, NULL);
    sceKernelStartThread(audioTh, 0, NULL);

    int sel = 0;
    int uiDirty = 1;
    int holdL = 0, holdR = 0;
    SceCtrlData pad, old;
    memset(&old, 0, sizeof(old));

    while (!done) {
        sceCtrlReadBufferPositive(&pad, 1);

        if ((pad.Buttons & PSP_CTRL_UP) && !(old.Buttons & PSP_CTRL_UP)) {
            sel = (sel + 6) % 7;  /* 0..6 */
            uiDirty = 1;
        }
        if ((pad.Buttons & PSP_CTRL_DOWN) && !(old.Buttons & PSP_CTRL_DOWN)) {
            sel = (sel + 1) % 7;
            uiDirty = 1;
        }

        float step = 0.01f;

        if (pad.Buttons & PSP_CTRL_LEFT) {
            holdL++;
            if (holdL > 30) step *= 10;
            else if (holdL > 10) step *= 5;
        } else holdL = 0;

        if (pad.Buttons & PSP_CTRL_RIGHT) {
            holdR++;
            if (holdR > 30) step *= 10;
            else if (holdR > 10) step *= 5;
        } else holdR = 0;

        if (sel == 0) {
            if ((pad.Buttons & PSP_CTRL_LEFT) && !(old.Buttons & PSP_CTRL_LEFT)) {
                g_currentPreset = (g_currentPreset + PRESET_COUNT - 1) % PRESET_COUNT;
                applyPreset(g_currentPreset);
                uiDirty = 1;
            }
            if ((pad.Buttons & PSP_CTRL_RIGHT) && !(old.Buttons & PSP_CTRL_RIGHT)) {
                g_currentPreset = (g_currentPreset + 1) % PRESET_COUNT;
                applyPreset(g_currentPreset);
                uiDirty = 1;
            }
        } else {
            float *p = NULL;
            switch (sel) {
                case 1: p = &g_attack;  break;
                case 2: p = &g_decay;   break;
                case 3: p = &g_sustain; break;
                case 4: p = &g_release; break;
                case 5: p = &g_cutoff;  break;
                case 6: p = &g_res;     break;
            }

            if (p) {
                if (holdL) { *p = clampf(*p - step, 0.0f, 1.0f); uiDirty = 1; }
                if (holdR) { *p = clampf(*p + step, 0.0f, 1.0f); uiDirty = 1; }
            }
        }

        if ((pad.Buttons & PSP_CTRL_LTRIGGER) && !(old.Buttons & PSP_CTRL_LTRIGGER)) {
            g_wave = (g_wave + WAVE_COUNT - 1) % WAVE_COUNT;
            uiDirty = 1;
        }
        if ((pad.Buttons & PSP_CTRL_RTRIGGER) && !(old.Buttons & PSP_CTRL_RTRIGGER)) {
            g_wave = (g_wave + 1) % WAVE_COUNT;
            uiDirty = 1;
        }

        if ((pad.Buttons & PSP_CTRL_TRIANGLE) && !(old.Buttons & PSP_CTRL_TRIANGLE)) {
            Preset *pr = &g_presets[g_currentPreset];
            pr->cutoff  = g_cutoff;
            pr->res     = g_res;
            pr->attack  = g_attack;
            pr->decay   = g_decay;
            pr->sustain = g_sustain;
            pr->release = g_release;
            pr->wave    = (int)g_wave;
            SavePresets();
            uiDirty = 1;
        }

        // if (pad.Buttons & PSP_CTRL_CROSS) {done = 1;}


        if (uiDirty) {
            pspDebugScreenClear();
            //pspDebugScreenPrintf("Preset: %d/%d (Triangle=Save)\n", g_currentPreset + 1, PRESET_COUNT);
            pspDebugScreenPrintf("Wave: %s\n\n", waveNames[g_wave]);

            //pspDebugScreenPrintf("%c PRESET\n",        sel==0?'>':' ');
            pspDebugScreenPrintf("%c PRESET %d/%d (Triangle=Save)\n", sel==0?'>':' ', g_currentPreset + 1, PRESET_COUNT);
            pspDebugScreenPrintf("%c ATTACK    %.2f\n",  sel==1?'>':' ', g_attack);
            pspDebugScreenPrintf("%c DECAY     %.2f\n",  sel==2?'>':' ', g_decay);
            pspDebugScreenPrintf("%c SUSTAIN   %.2f\n",  sel==3?'>':' ', g_sustain);
            pspDebugScreenPrintf("%c RELEASE   %.2f\n",  sel==4?'>':' ', g_release);
            pspDebugScreenPrintf("%c CUTOFF    %.2f\n",  sel==5?'>':' ', g_cutoff);
            pspDebugScreenPrintf("%c RESONANCE %.2f\n",  sel==6?'>':' ', g_res);

            pspDebugScreenSetXY(40, 33);
            pspDebugScreenPrintf("PSP Synth made by AlexKaut");

            uiDirty = 0;
        }

        old = pad;
        sceKernelDelayThread(10000);
    }

    sceKernelExitGame();
    return 0;
}
