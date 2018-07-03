/**
 * A quad hut finder with lots of fancy options.
 */

#include "finders.h"
#include "generator.h"
#include "layers.h"

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <string.h>
#include <unistd.h>


typedef struct {
    int numMonuments;
    Pos monuments[4];
} Monuments;

enum BiomeConfigs {
    noneCfg = -1,
    oceanCfg = 0,
    flowerForestCfg,
    iceSpikesCfg,
    jungleCfg,
    megaTaigaCfg,
    mesaCfg,
    mushroomIslandCfg,
};
#define NUM_BIOME_SEARCH_CONFIGS 10

typedef struct {
    char name[20];
    float fraction;
    int lookup[256];
} BiomeSearchConfig;
BiomeSearchConfig biomeSearchConfigs[NUM_BIOME_SEARCH_CONFIGS];

typedef struct {
    int radius;  /* Search radius in blocks. */
    int hutRadius;
    int mansionRadius;
    long startSeed;
    long endSeed;
    int threads;
    char outputDir[256];
    char baseSeedsFile[256];
    BiomeSearchConfig *spawnBiomes;
    int monumentDistance;
    int woodlandMansions;
} SearchOptions;

typedef struct {
    int thread;
    int startIndex;
    const long *qhcandidates;
    long qhcount;
    const SearchOptions *opts;
    char filename[256];
} ThreadInfo;

#define INT_ERROR "An integer argument is required with --%s\n"


void initSearchConfig(
        char *name, BiomeSearchConfig *config, float fraction,
        int includedCount, int *includedBiomes,
        int ignoredCount, int *ignoredBiomes) {
    snprintf(config->name, 20, "%s", name);
    config->fraction = fraction;

    memset(config->lookup, 0, 256*sizeof(int));
    for (int i=0; i<includedCount; i++) {
        assert(includedBiomes[i] < 256);
        config->lookup[includedBiomes[i]] = 1;
    }
    for (int i=0; i<ignoredCount; i++) {
        assert(ignoredBiomes[i] < 256);
        config->lookup[ignoredBiomes[i]] = -1;
    }
}


void initSearchConfigs() {
    initSearchConfig(
            "ocean",
            &biomeSearchConfigs[oceanCfg], 0.85f,
            3, (int[]){ocean, frozenOcean, deepOcean},
            0, (int[]){});

    initSearchConfig(
            "flower forest",
            &biomeSearchConfigs[flowerForestCfg], 0.65f,
            1, (int[]){forest+128},
            3, (int[]){river, ocean, deepOcean});

    initSearchConfig(
            "ice spikes",
            &biomeSearchConfigs[iceSpikesCfg], 0.75f,
            1, (int[]){icePlains+128},
            7, (int[]){icePlains, iceMountains, frozenRiver,
                       river, frozenOcean, ocean, deepOcean});

    initSearchConfig(
            "jungle",
            &biomeSearchConfigs[jungleCfg], 0.95f,
            5, (int[]){jungle, jungleHills, jungleEdge, jungle+128, jungleEdge+128},
            3, (int[]){river, ocean, deepOcean});

    initSearchConfig(
            "mega taiga",
            &biomeSearchConfigs[megaTaigaCfg], 0.90f,
            4, (int[]){megaTaiga, megaTaigaHills,
                       megaTaiga+128, megaTaigaHills+128},
            3, (int[]){river, ocean, deepOcean});

    initSearchConfig(
            "mesa",
            &biomeSearchConfigs[mesaCfg], 0.90f,
            6, (int[]){mesa, mesaPlateau_F, mesaPlateau,
                       mesa+128, mesaPlateau_F+128, mesaPlateau+128},
            3, (int[]){river, ocean, deepOcean});

    initSearchConfig(
            "mushroom island",
            &biomeSearchConfigs[mushroomIslandCfg], 0.50f,
            2, (int[]){mushroomIsland, mushroomIslandShore},
            3, (int[]){river, ocean, deepOcean});
}


void usage() {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  multifinder [options]\n");
    fprintf(stderr, "    --help\n");
    fprintf(stderr, "    --radius=<integer>\n");
    fprintf(stderr, "      Search radius, in blocks (rounded to nearest\n");
    fprintf(stderr, "      structure region).\n");
    fprintf(stderr, "    --start_seed=<integer>\n");
    fprintf(stderr, "    --end_seed=<integer>\n");
    fprintf(stderr, "    --threads=<integer>\n");
    fprintf(stderr, "    --output_dir=<string>\n");
    fprintf(stderr, "    --spawn_biomes=<string>\n");
    fprintf(stderr, "      ocean, flower_forest, ice_spikes, jungle,\n");
    fprintf(stderr, "      mega_taiga, mesa or mushroom_island.\n");
    fprintf(stderr, "    --monument_distance=<integer>\n");
    fprintf(stderr, "      Search for an ocean monument within a number of\n");
    fprintf(stderr, "      chunks of the quad hut perimeter.\n");
}


long parseHumanArgument(char *arg, const char *flagName) {
    char *endptr;

    int len = strlen(arg);
    if (len < 1) {
        fprintf(stderr, INT_ERROR, flagName);
        exit(-1);
    }

    long mult = 1;
    switch (arg[len-1]) {
        case 'K': mult = 1024L; break;
        case 'M': mult = 1024L*1024L; break;
        case 'B': mult = 1024L*1024L*1024L; break;
        case 'G': mult = 1024L*1024L*1024L; break;
        case 'T': mult = 1024L*1024L*1024L*1024L; break;
    }

    if (mult != 1)
        arg[len-1] = 0;
    long val = strtol(arg, &endptr, 10);
    if (errno != 0) {
        fprintf(stderr, INT_ERROR, flagName);
        exit(-1);
    }

    return val*mult;
}


int parseIntArgument(const char *arg, const char *flagName) {
    char *endptr;

    int val = strtol(arg, &endptr, 10);
    if (errno != 0) {
        fprintf(stderr, INT_ERROR, flagName);
        exit(-1);
    }

    return val;
}


BiomeSearchConfig* parseSpawnBiome(const char *arg) {
    if (strcmp(arg, "ocean")              == 0)
        return &biomeSearchConfigs[oceanCfg];

    if (strcmp(arg, "flower_forest")      == 0 ||
            strcmp(arg, "flower")         == 0 ||
            strcmp(arg, "flowerForest")   == 0)
        return &biomeSearchConfigs[flowerForestCfg];

    if (strcmp(arg, "ice_spikes")         == 0 ||
            strcmp(arg, "iceSpikes")      == 0)
        return &biomeSearchConfigs[iceSpikesCfg];

    if (strcmp(arg, "jungle")             == 0)
        return &biomeSearchConfigs[jungleCfg];

    if (strcmp(arg, "mega_taiga")         == 0 ||
            strcmp(arg, "megaTaiga")      == 0)
        return &biomeSearchConfigs[megaTaigaCfg];

    if (strcmp(arg, "mesa")               == 0)
        return &biomeSearchConfigs[mesaCfg];

    if (strcmp(arg, "mushroom_island")    == 0 ||
            strcmp(arg, "mushroom")       == 0 ||
            strcmp(arg, "mushroomIsland") == 0)
        return &biomeSearchConfigs[mushroomIslandCfg];

    fprintf(stderr, "Unknown biome group \"%s\".\n", arg);
    exit(-1);
}


SearchOptions parseOptions(int argc, char *argv[]) {
    int c;
    SearchOptions opts = {
        .radius           = 2048,
        .hutRadius        = 4,
        .mansionRadius    = 2,
        .startSeed        = 0,
        .endSeed          = 1L<<48,
        .threads          = 1,
        .outputDir        = "",
        .baseSeedsFile    = "./seeds/quadbases_Q1.txt",
        .spawnBiomes      = NULL,
        .monumentDistance = 0,
        .woodlandMansions = 0,
    };

    while (1) {
        static struct option longOptions[] = {
            {"radius",            required_argument, NULL, 'r'},
            {"start_seed",        required_argument, NULL, 's'},
            {"end_seed",          required_argument, NULL, 'e'},
            {"threads",           required_argument, NULL, 't'},
            {"output_dir",        required_argument, NULL, 'o'},
            {"base_seeds_file",   required_argument, NULL, 'S'},
            {"spawn_biomes",      required_argument, NULL, 'b'},
            {"monument_distance", required_argument, NULL, 'm'},
            {"woodland_mansions", required_argument, NULL, 'w'},
            {"help",              no_argument,       NULL, 'h'},
        };
        int index = 0;
        c = getopt_long(argc, argv, "r:s:e:t:o:S:b:m:w:h", longOptions, &index);

        if (c == -1)
            break;

        switch (c) {
            case 'r':
                opts.radius = parseIntArgument(
                        optarg, longOptions[index].name);
                opts.hutRadius = (int)ceil((double)opts.radius / (32*16));
                opts.mansionRadius = (int)ceil((double)opts.radius / (80*16));
                break;
            case 's':
                opts.startSeed = parseHumanArgument(
                        optarg, longOptions[index].name);
                break;
            case 'e':
                opts.endSeed = parseHumanArgument(
                        optarg, longOptions[index].name);
                break;
            case 't':
                opts.threads = parseIntArgument(
                        optarg, longOptions[index].name);
                break;
            case 'o':
                if (strlen(optarg) > 255-13) {
                    fprintf(stderr, "Output path too long.");
                    exit(-1);
                }
                strncpy(opts.outputDir, optarg, 256);
                int len = strlen(opts.outputDir);
                if (opts.outputDir[len-1] == '/')
                    opts.outputDir[len-1] = 0;
                break;
            case 'S':
                if (strlen(optarg) > 255) {
                    fprintf(stderr, "Base seeds filename too long.");
                    exit(-1);
                }
                strncpy(opts.baseSeedsFile, optarg, 256);
                break;
            case 'b':
                opts.spawnBiomes = parseSpawnBiome(optarg);
                break;
            case 'm':
                opts.monumentDistance = parseIntArgument(
                        optarg, longOptions[index].name);
                break;
            case 'w':
                opts.woodlandMansions = parseIntArgument(
                        optarg, longOptions[index].name);
                break;
            case 'h':
                usage();
                exit(0);
                break;
            default:
                exit(-1);
        }
    }
    return opts;
}


long* getBaseSeeds(long *qhcount, int threads, const char *seedFileName) {
    if (access(seedFileName, F_OK)) {
        fprintf(stderr, "Seed base file does not exist: Creating new one.\n"
                "This may take a few minutes...\n");
        int quality = 1;
        baseQuadWitchHutSearch(seedFileName, threads, quality);
    }

    return loadSavedSeeds(seedFileName, qhcount);
}


int getBiomeAt(const LayerStack g, const Pos pos, int *buf) {
    genArea(&g.layers[g.layerNum-1], buf, pos.x, pos.z, 1, 1);
    return buf[0];
}


Monuments potentialMonuments(long baseSeed, int distance) {
    const int upper = 23 - distance;
    const int lower = distance;
    Monuments potential;
    potential.numMonuments = 0;
    Pos pos;

    pos = getOceanMonumentChunk(baseSeed, 0, 0);
    if (pos.x >= upper && pos.z >= upper) {
        pos.x = (pos.x +  0) * 16 + 8;
        pos.z = (pos.z +  0) * 16 + 8;
        potential.monuments[potential.numMonuments++] = pos;
    }

    pos = getOceanMonumentChunk(baseSeed, 1, 0);
    if (pos.x <= lower && pos.z >= upper) {
        pos.x = (pos.x + 32) * 16 + 8;
        pos.z = (pos.z +  0) * 16 + 8;
        potential.monuments[potential.numMonuments++] = pos;
    }

    pos = getOceanMonumentChunk(baseSeed, 0, 1);
    if (pos.x >= upper && pos.z <= lower) {
        pos.x = (pos.x +  0) * 16 + 8;
        pos.z = (pos.z + 32) * 16 + 8;
        potential.monuments[potential.numMonuments++] = pos;
    }

    pos = getOceanMonumentChunk(baseSeed, 1, 1);
    if (pos.x <= lower && pos.z <= lower) {
        pos.x = (pos.x + 32) * 16 + 8;
        pos.z = (pos.z + 32) * 16 + 8;
        potential.monuments[potential.numMonuments++] = pos;
    }

    return potential;
}


int verifyMonuments(LayerStack *g, Monuments *mon, int rX, int rZ) {
    for (int m = 0; m < mon->numMonuments; m++) {
        // Translate monument coordintes from the origin-relative coordinates
        // from the base seed family.
        int monX = mon->monuments[m].x + rX*32*16;
        int monZ = mon->monuments[m].z + rZ*32*16;
        if (isViableOceanMonumentPos(*g, NULL, monX, monZ)) {
            return 1;
        }
    }
    return 0;
}


int hasMansions(const LayerStack *g, long seed, int radius, int minCount) {
    int count = 0;
    for (int rZ=-radius; rZ<radius; rZ++) {
        for (int rX=-radius; rX<radius; rX++) {
            Pos mansion = getMansionPos(seed, rX, rZ);
            // TODO: Preallocate the cache?
            if (isViableMansionPos(*g, NULL, mansion.x, mansion.z)) {
                count++;
                if (count >= minCount) {
                    return 1;
                }
            }
        }
    }
    return 0;
}


int hasSpawnBiome(LayerStack *g, Pos spawn, BiomeSearchConfig *config) {
    Layer *lShoreBiome = &g->layers[L_SHORE_16];

    // Shore biome is 16:1, and spawn is 256x256, and we want to include
    // the neighboring areas which blend into it -> 18.
    // TODO: Might be a bit better to allocate this once.
    int *spawnCache = allocCache(lShoreBiome, 18, 18);
    int areaX = spawn.x >> 4;
    int areaZ = spawn.z >> 4;
    float ignoreFraction = 0;
    float includeFraction = 0;

    genArea(lShoreBiome, spawnCache, areaX-9, areaZ-9, 18, 18);

    for (int i=0; i<18*18; i++) {
        switch (config->lookup[spawnCache[i]]) {
            case 1:
                includeFraction += 1;
                break;
            case -1:
                ignoreFraction += 1;
                break;
        }
    }

    free(spawnCache);

    includeFraction /= (18*18);
    ignoreFraction /= (18*18);
    if (ignoreFraction > 0.80f) { ignoreFraction = 0.80f; }

    return includeFraction / (1.0 - ignoreFraction) >= config->fraction;
}


void *searchQuadHutsThread(void *data) {
    const ThreadInfo info = *(const ThreadInfo *)data;
    const SearchOptions opts = *info.opts;

    LayerStack g = setupGenerator();
    Layer *lFilterBiome = &g.layers[L_BIOME_256];
    int *biomeCache = allocCache(lFilterBiome, 3, 3);
    int *lastLayerCache = allocCache(&g.layers[g.layerNum-1], 3, 3);
    long j, base, seed;

    Monuments monuments;

    // Load the positions of the four structures that make up the quad-structure
    // so we can test the biome at these positions.
    Pos qhpos[4];

    // Setup a dummy layer for Layer 19: Biome.
    Layer layerBiomeDummy;
    setupLayer(256, &layerBiomeDummy, NULL, 200, NULL);

    FILE *fh;
    if (strlen(info.filename)) {
        fh = fopen(info.filename, "w");
        if (fh == NULL) {
            fprintf(stderr, "Could not open file %s.\n", info.filename);
            return NULL;
        }
    } else {
        fh = stdout;
    }

    // Every nth + m base seed is assigned to thread m;
    for(int i=info.startIndex;
            i < info.qhcount && info.qhcandidates[i] < opts.endSeed;
            i+=opts.threads) {
        int basehits = 0;

        // The ocean monument check is quick and has a high probability
        // of eliminating the seed, so perform that first.
        if (opts.monumentDistance) {
            monuments = potentialMonuments(
                    info.qhcandidates[i], opts.monumentDistance);
            if (monuments.numMonuments == 0)
                continue;
        }

        for (int rZ = -opts.hutRadius-1; rZ < opts.hutRadius; rZ++) {
            for (int rX = -opts.hutRadius-1; rX < opts.hutRadius; rX++) {
                // The base seed has potential monuments around the origin;
                // if we translate it to rX, rZ, it will always have potential
                // huts around that region.
                base = moveTemple(info.qhcandidates[i], rX, rZ);

                // rZ, rX is the hut region in the upper left of the potential
                // quad hut. Hut regions are 32 chunks/512 blocks. The biome
                // generation layers we're looking at are 1:256 zoom. So
                // the biome area is 2* the hut region. Also, we want the area
                // at the center of the quad-hut regions, so +1.
                int areaX = (rX << 1) + 1;
                int areaZ = (rZ << 1) + 1;

                // This little magic code checks if there is a meaningful chance
                // for this seed base to generate swamps in the area.

                // The idea is that the conversion from Lush temperature to
                // swampland is independent of surroundings, so we can test the
                // conversion beforehand. Furthermore biomes tend to leak into
                // the negative coordinates because of the Zoom layers, so the
                // majority of hits will occur when SouthEast corner (at a 1:256
                // scale) of the quad-hut has a swampland. (This assumption
                // misses about 1 in 500 quad-hut seeds.) Finally, here we also
                // exploit that the minecraft random number generator is quite
                // bad, such that for the "mcNextRand() mod 6" check it has a
                // period pattern of ~3 on the high seed-bits.

                // Misses 8-9% of seeds for a 2x speedup.
                for (j = 0x53; j < 0x58; j++) {
                    seed = base + (j << 48);
                    setWorldSeed(&layerBiomeDummy, seed);
                    setChunkSeed(&layerBiomeDummy, areaX+1, areaZ+1);
                    if(mcNextInt(&layerBiomeDummy, 6) == 5)
                        break;
                }
                if (j >= 0x58)
                    continue;

                qhpos[0] = getWitchHutPos(base, 0+rX, 0+rZ);
                qhpos[1] = getWitchHutPos(base, 0+rX, 1+rZ);
                qhpos[2] = getWitchHutPos(base, 1+rX, 0+rZ);
                qhpos[3] = getWitchHutPos(base, 1+rX, 1+rZ);

                long hits = 0;
                long hutHits = 0;

                for (j = 0; j < 0x10000; j++) {
                    seed = base + (j << 48);
                    setWorldSeed(&layerBiomeDummy, seed);

                    // This seed base does not seem to contain many quad huts,
                    // so make a more detailed analysis of the surroundings and
                    // see if there is enough potential for more swamps to
                    // justify searching further. Misses and additional 1% of
                    // seeds for a 1.4:1 speedup.

                    // This uses a separate counter for all seeds that pass the
                    // quad hut checks, even if they fail the other checks, so
                    // that the other checks don't cause this to fail too early.
                    if(hutHits == 0 && (j & 0xfff) == 0xfff) {
                        int swpc = 0;
                        setChunkSeed(&layerBiomeDummy, areaX, areaZ+1);
                        swpc += mcNextInt(&layerBiomeDummy, 6) == 5;
                        setChunkSeed(&layerBiomeDummy, areaX+1, areaZ);
                        swpc += mcNextInt(&layerBiomeDummy, 6) == 5;
                        setChunkSeed(&layerBiomeDummy, areaX, areaZ);
                        swpc += mcNextInt(&layerBiomeDummy, 6) == 5;

                        if (swpc < (j > 0x1000 ? 2 : 1))
                            break;
                    }

                    // We can check that at least one swamp could generate in
                    // this area before doing the biome generator checks.
                    // Misses an additional 0.2% of seeds for a 2.75:1 speedup.
                    setChunkSeed(&layerBiomeDummy, areaX+1, areaZ+1);
                    if (mcNextInt(&layerBiomeDummy, 6) != 5)
                        continue;

                    // Dismiss seeds that don't have a swamp near the quad
                    // temple. Misses an additional 0.03% of seeds for a 1.7:1
                    // speedup.
                    setWorldSeed(lFilterBiome, seed);
                    genArea(lFilterBiome, biomeCache, areaX+1, areaZ+1, 1, 1);
                    if (biomeCache[0] != swampland)
                        continue;

                    applySeed(&g, seed);
                    if (getBiomeAt(g, qhpos[0], lastLayerCache) != swampland)
                        continue;
                    if (getBiomeAt(g, qhpos[1], lastLayerCache) != swampland)
                        continue;
                    if (getBiomeAt(g, qhpos[2], lastLayerCache) != swampland)
                        continue;
                    if (getBiomeAt(g, qhpos[3], lastLayerCache) != swampland)
                        continue;
                    hutHits++;

                    // This check has to get exact biomes for a whole area, so
                    // is relatively slow. It might be a bit faster if we
                    // preallocate a cache and stuff, but it might be marginal.
                    if (opts.monumentDistance &&
                            !verifyMonuments(&g, &monuments, rX, rZ))
                        continue;

                    if (opts.woodlandMansions &&
                            !hasMansions(&g, seed, opts.mansionRadius, opts.woodlandMansions))
                        continue;

                    if (opts.spawnBiomes) {
                        // TODO: Preallocate cache?
                        Pos spawn = getSpawn(&g, NULL, seed);
                        if (!hasSpawnBiome(&g, spawn, opts.spawnBiomes))
                            continue;
                    }

                    fprintf(fh, "%ld\n", seed);
                    hits++;
                    basehits++;
                }
                fflush(fh);
            }
        }
        fprintf(stderr, "Base seed %ld (thread %d): %d hits\n",
                info.qhcandidates[i], info.thread, basehits);
    }

    if (fh != stdout) {
        fclose(fh);
        fprintf(stderr, "%s written.\n", info.filename);
    }
    free(biomeCache);
    free(lastLayerCache);
    freeGenerator(g);

    return NULL;
}


int main(int argc, char *argv[])
{
    // Always initialize the biome list before starting any seed finder or
    // biome generator.
    initBiomes();
    initSearchConfigs();

    SearchOptions opts = parseOptions(argc, argv);

    if (opts.threads > 1 && strlen(opts.outputDir) < 1) {
        fprintf(stderr,
                "Must specify --output_dir if using more than one thread.");
        exit(-1);
    }

    fprintf(stderr, "===========================================================================\n");
    fprintf(stderr,
            "Searching base seeds %ld-%ld, radius %d using %d threads...\n",
            opts.startSeed, opts.endSeed, opts.radius, opts.threads);
    if (opts.monumentDistance) {
        fprintf(stderr, "Want an ocean monument within %d chunks of quad hut perimeter.\n", opts.monumentDistance);
    }
    if (opts.woodlandMansions) {
        fprintf(stderr, "Want %d woodland mansions within the search radius.\n", opts.woodlandMansions);
    }
    if (opts.spawnBiomes) {
        fprintf(stderr, "Looking for world spawn in %s biomes.\n", opts.spawnBiomes->name);
    }
    fprintf(stderr, "===========================================================================\n");

    long qhcount;
    const long *qhcandidates = getBaseSeeds(&qhcount, opts.threads, opts.baseSeedsFile);
    int startIndex = 0;
    while (qhcandidates[startIndex] < opts.startSeed && startIndex < qhcount) {
        startIndex++;
    }

    pthread_t threadID[opts.threads];
    ThreadInfo info[opts.threads];

    for (int t=0; t<opts.threads; t++) {
        info[t].thread = t;
        info[t].startIndex = startIndex + t;
        info[t].qhcandidates = qhcandidates;
        info[t].qhcount = qhcount;
        info[t].opts = &opts;

        if (opts.threads == 1 && !strlen(opts.outputDir)) {
            info[t].filename[0] = 0;
        } else {
            snprintf(info[t].filename, 256,
                    "%s/seeds-%02d.txt", opts.outputDir, t);
        }
    }

    for (int t=0; t<opts.threads; t++) {
        pthread_create(
                &threadID[t], NULL, searchQuadHutsThread, (void*)&info[t]);
    }

    for (int t=0; t<opts.threads; t++) {
        pthread_join(threadID[t], NULL);
    }

    if (strlen(opts.outputDir)) {
        char filename[256];
        snprintf(filename, 256, "%s/COMPLETE", opts.outputDir);
        FILE *fh = fopen(filename, "w");
        if (fh != NULL) {
            fprintf(fh, "Done.\n");
            fclose(fh);
        }
    }
    fprintf(stderr, "Done.\n");

    return 0;
}