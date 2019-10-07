/*
 *
 * honggfuzz - file operations
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2018 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libhfcommon/common.h"
#include "libhfcommon/files.h"
#include "libhfcommon/log.h"
#include "libhfcommon/util.h"
#include "mangle.h"
#include "subproc.h"

void input_setSize(run_t* run, size_t sz) {
    if (run->dynamicFileSz == sz) {
        return;
    }
    if (sz > run->global->mutate.maxFileSz) {
        PLOG_F("Too large size requested: %zu > maxSize: %zu", sz, run->global->mutate.maxFileSz);
    }
    /* ftruncate of a mmaped file fails under CygWin, it's also painfully slow under MacOS X */
#if !defined(__CYGWIN__) && !defined(_HF_ARCH_DARWIN)
    if (TEMP_FAILURE_RETRY(ftruncate(run->dynamicFileFd, sz)) == -1) {
        PLOG_W("ftruncate(run->dynamicFileFd=%d, sz=%zu)", run->dynamicFileFd, sz);
    }
#endif /* !defined(__CYGWIN__) && !defined(_HF_ARCH_DARWIN) */
    run->dynamicFileSz = sz;
}

static bool input_getDirStatsAndRewind(honggfuzz_t* hfuzz) {
    rewinddir(hfuzz->io.inputDirPtr);

    size_t maxSize = 0U;
    size_t fileCnt = 0U;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(hfuzz->io.inputDirPtr);
        if (entry == NULL && errno == EINTR) {
            continue;
        }
        if (entry == NULL && errno != 0) {
            PLOG_W("readdir('%s')", hfuzz->io.inputDir);
            return false;
        }
        if (entry == NULL) {
            break;
        }

        char fname[PATH_MAX];
        snprintf(fname, sizeof(fname), "%s/%s", hfuzz->io.inputDir, entry->d_name);
        LOG_D("Analyzing file '%s'", fname);

        struct stat st;
        if (stat(fname, &st) == -1) {
            LOG_W("Couldn't stat() the '%s' file", fname);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            LOG_D("'%s' is not a regular file, skipping", fname);
            continue;
        }
        if (hfuzz->mutate.maxFileSz != 0UL && st.st_size > (off_t)hfuzz->mutate.maxFileSz) {
            LOG_D("File '%s' is bigger than maximal defined file size (-F): %" PRId64 " > %" PRId64,
                fname, (int64_t)st.st_size, (int64_t)hfuzz->mutate.maxFileSz);
        }
        if ((size_t)st.st_size > maxSize) {
            maxSize = st.st_size;
        }
        fileCnt++;
    }

    ATOMIC_SET(hfuzz->io.fileCnt, fileCnt);
    if (hfuzz->mutate.maxFileSz == 0U) {
        if (maxSize < 8192) {
            hfuzz->mutate.maxFileSz = 8192;
        } else if (maxSize > _HF_INPUT_MAX_SIZE) {
            hfuzz->mutate.maxFileSz = _HF_INPUT_MAX_SIZE;
        } else {
            hfuzz->mutate.maxFileSz = maxSize;
        }
    }

    if (hfuzz->io.fileCnt == 0U) {
        LOG_W("No usable files in the input directory '%s'", hfuzz->io.inputDir);
    }

    LOG_D("Re-read the '%s', maxFileSz:%zu, number of usable files:%zu", hfuzz->io.inputDir,
        hfuzz->mutate.maxFileSz, hfuzz->io.fileCnt);

    rewinddir(hfuzz->io.inputDirPtr);

    return true;
}

bool input_getNext(run_t* run, char* fname, bool rewind) {
    static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
    MX_SCOPED_LOCK(&input_mutex);

    if (run->global->io.fileCnt == 0U) {
        LOG_W("No useful files in the input directory");
        return false;
    }

    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(run->global->io.inputDirPtr);
        if (entry == NULL && errno == EINTR) {
            continue;
        }
        if (entry == NULL && errno != 0) {
            PLOG_W("readdir_r('%s')", run->global->io.inputDir);
            return false;
        }
        if (entry == NULL && rewind == false) {
            return false;
        }
        if (entry == NULL && rewind == true) {
            if (input_getDirStatsAndRewind(run->global) == false) {
                LOG_E("input_getDirStatsAndRewind('%s')", run->global->io.inputDir);
                return false;
            }
            continue;
        }

        snprintf(fname, PATH_MAX, "%s/%s", run->global->io.inputDir, entry->d_name);
        struct stat st;
        if (stat(fname, &st) == -1) {
            LOG_W("Couldn't stat() the '%s' file", fname);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            LOG_D("'%s' is not a regular file, skipping", fname);
            continue;
        }
        return true;
    }
}

bool input_init(honggfuzz_t* hfuzz) {
    hfuzz->io.fileCnt = 0U;

    if (!hfuzz->io.inputDir) {
        LOG_W("No input file/dir specified");
        return false;
    }

    int dir_fd = TEMP_FAILURE_RETRY(open(hfuzz->io.inputDir, O_DIRECTORY | O_RDONLY | O_CLOEXEC));
    if (dir_fd == -1) {
        PLOG_W("open('%s', O_DIRECTORY|O_RDONLY|O_CLOEXEC)", hfuzz->io.inputDir);
        return false;
    }
    if ((hfuzz->io.inputDirPtr = fdopendir(dir_fd)) == NULL) {
        PLOG_W("fdopendir(dir='%s', fd=%d)", hfuzz->io.inputDir, dir_fd);
        close(dir_fd);
        return false;
    }
    if (input_getDirStatsAndRewind(hfuzz) == false) {
        hfuzz->io.fileCnt = 0U;
        LOG_W("input_getDirStatsAndRewind('%s')", hfuzz->io.inputDir);
        return false;
    }

    return true;
}

bool input_parseDictionary(honggfuzz_t* hfuzz) {
    FILE* fDict = fopen(hfuzz->mutate.dictionaryFile, "rb");
    if (fDict == NULL) {
        PLOG_W("Couldn't open '%s' - R/O mode", hfuzz->mutate.dictionaryFile);
        return false;
    }
    defer {
        fclose(fDict);
    };

    char* lineptr = NULL;
    size_t n = 0;
    defer {
        free(lineptr);
    };
    for (;;) {
        ssize_t len = getdelim(&lineptr, &n, '\n', fDict);
        if (len == -1) {
            break;
        }
        if (len > 1 && lineptr[len - 1] == '\n') {
            lineptr[len - 1] = '\0';
            len--;
        }
        if (lineptr[0] == '#') {
            continue;
        }
        if (lineptr[0] == '\n') {
            continue;
        }
        if (lineptr[0] == '\0') {
            continue;
        }
        char bufn[1025] = {};
        char bufv[1025] = {};
        if (sscanf(lineptr, "\"%1024[^\"]", bufv) != 1 &&
            sscanf(lineptr, "%1024[^=]=\"%1024[^\"]", bufn, bufv) != 2) {
            LOG_W("Incorrect dictionary entry: '%s'. Skipping", lineptr);
            continue;
        }

        LOG_D("Parsing word: '%s'", bufv);

        len = util_decodeCString(bufv);
        struct strings_t* str = (struct strings_t*)util_Malloc(sizeof(struct strings_t) + len + 1);
        memcpy(str->s, bufv, len);
        str->len = len;
        str->s[len] = '\0';
        hfuzz->mutate.dictionaryCnt += 1;
        TAILQ_INSERT_TAIL(&hfuzz->mutate.dictq, str, pointers);

        LOG_D("Dictionary: loaded word: '%s' (len=%zu)", str->s, str->len);
    }
    LOG_I("Loaded %zu words from the dictionary", hfuzz->mutate.dictionaryCnt);
    return true;
}

bool input_parseBlacklist(honggfuzz_t* hfuzz) {
    FILE* fBl = fopen(hfuzz->feedback.blacklistFile, "rb");
    if (fBl == NULL) {
        PLOG_W("Couldn't open '%s' - R/O mode", hfuzz->feedback.blacklistFile);
        return false;
    }
    defer {
        fclose(fBl);
    };

    char* lineptr = NULL;
    /* lineptr can be NULL, but it's fine for free() */
    defer {
        free(lineptr);
    };
    size_t n = 0;
    for (;;) {
        if (getline(&lineptr, &n, fBl) == -1) {
            break;
        }

        if ((hfuzz->feedback.blacklist = util_Realloc(hfuzz->feedback.blacklist,
                 (hfuzz->feedback.blacklistCnt + 1) * sizeof(hfuzz->feedback.blacklist[0]))) ==
            NULL) {
            PLOG_W("realloc failed (sz=%zu)",
                (hfuzz->feedback.blacklistCnt + 1) * sizeof(hfuzz->feedback.blacklist[0]));
            return false;
        }

        hfuzz->feedback.blacklist[hfuzz->feedback.blacklistCnt] = strtoull(lineptr, 0, 16);
        LOG_D("Blacklist: loaded %'" PRIu64 "'",
            hfuzz->feedback.blacklist[hfuzz->feedback.blacklistCnt]);

        /* Verify entries are sorted so we can use interpolation search */
        if (hfuzz->feedback.blacklistCnt > 1) {
            if (hfuzz->feedback.blacklist[hfuzz->feedback.blacklistCnt - 1] >
                hfuzz->feedback.blacklist[hfuzz->feedback.blacklistCnt]) {
                LOG_F("Blacklist file not sorted. Use 'tools/createStackBlacklist.sh' to sort "
                      "records");
                return false;
            }
        }
        hfuzz->feedback.blacklistCnt += 1;
    }

    if (hfuzz->feedback.blacklistCnt > 0) {
        LOG_I("Loaded %zu stack hash(es) from the blacklist file", hfuzz->feedback.blacklistCnt);
    } else {
        LOG_F("Empty stack hashes blacklist file '%s'", hfuzz->feedback.blacklistFile);
    }
    return true;
}

bool input_prepareDynamicInput(run_t* run, bool needs_mangle) {
    struct dynfile_t* current = NULL;

    {
        MX_SCOPED_RWLOCK_WRITE(&run->global->io.dynfileq_mutex);

        if (run->global->io.dynfileqCnt == 0) {
            LOG_F("The dynamic file corpus is empty. This shouldn't happen");
        }

        if (run->global->io.dynfileqCurrent == NULL) {
            run->global->io.dynfileqCurrent = TAILQ_FIRST(&run->global->io.dynfileq);
        }
        current = run->global->io.dynfileqCurrent;
        run->global->io.dynfileqCurrent = TAILQ_NEXT(run->global->io.dynfileqCurrent, pointers);
    }

    input_setSize(run, current->size);
    memcpy(run->dynamicFile, current->data, current->size);

    if (needs_mangle) {
        mangle_mangleContent(run);
    }

    return true;
}

bool input_prepareStaticFile(run_t* run, bool rewind, bool needs_mangle) {
    char fname[PATH_MAX];
    if (!input_getNext(run, fname, /* rewind= */ rewind)) {
        return false;
    }
    snprintf(run->origFileName, sizeof(run->origFileName), "%s", fname);

    input_setSize(run, run->global->mutate.maxFileSz);
    ssize_t fileSz = files_readFileToBufMax(fname, run->dynamicFile, run->global->mutate.maxFileSz);
    if (fileSz < 0) {
        LOG_E("Couldn't read contents of '%s'", fname);
        return false;
    }

    input_setSize(run, fileSz);
    if (needs_mangle) {
        mangle_mangleContent(run);
    }

    return true;
}

void input_removeStaticFile(const char* path) {
    if (unlink(path) == -1) {
        PLOG_E("unlink('%s') failed", path);
    }
}

bool input_prepareExternalFile(run_t* run) {
    snprintf(run->origFileName, sizeof(run->origFileName), "[EXTERNAL]");

    int fd = files_writeBufToTmpFile(run->global->io.workDir, (const uint8_t*)"", 0, 0);
    if (fd == -1) {
        LOG_E("Couldn't write input file to a temporary buffer");
        return false;
    }
    defer {
        close(fd);
    };

    char fname[PATH_MAX];
    snprintf(fname, sizeof(fname), "/dev/fd/%d", fd);

    const char* const argv[] = {run->global->exe.externalCommand, fname, NULL};
    if (subproc_System(run, argv) != 0) {
        LOG_E("Subprocess '%s' returned abnormally", run->global->exe.externalCommand);
        return false;
    }
    LOG_D("Subporcess '%s' finished with success", run->global->exe.externalCommand);

    input_setSize(run, run->global->mutate.maxFileSz);
    ssize_t sz = files_readFromFdSeek(fd, run->dynamicFile, run->global->mutate.maxFileSz, 0);
    if (sz == -1) {
        LOG_E("Couldn't read file from fd=%d", fd);
        return false;
    }

    input_setSize(run, (size_t)sz);
    return true;
}

bool input_postProcessFile(run_t* run) {
    int fd =
        files_writeBufToTmpFile(run->global->io.workDir, run->dynamicFile, run->dynamicFileSz, 0);
    if (fd == -1) {
        LOG_E("Couldn't write input file to a temporary buffer");
        return false;
    }
    defer {
        close(fd);
    };

    char fname[PATH_MAX];
    snprintf(fname, sizeof(fname), "/dev/fd/%d", fd);

    const char* const argv[] = {run->global->exe.postExternalCommand, fname, NULL};
    if (subproc_System(run, argv) != 0) {
        LOG_E("Subprocess '%s' returned abnormally", run->global->exe.postExternalCommand);
        return false;
    }
    LOG_D("Subporcess '%s' finished with success", run->global->exe.externalCommand);

    input_setSize(run, run->global->mutate.maxFileSz);
    ssize_t sz = files_readFromFdSeek(fd, run->dynamicFile, run->global->mutate.maxFileSz, 0);
    if (sz == -1) {
        LOG_E("Couldn't read file from fd=%d", fd);
        return false;
    }

    input_setSize(run, (size_t)sz);
    return true;
}

bool input_prepareDynamicFileForMinimization(run_t* run) {
    MX_SCOPED_RWLOCK_WRITE(&run->global->io.dynfileq_mutex);

    if (run->global->io.dynfileqCnt == 0) {
        LOG_F("The dynamic file corpus is empty (for minimization). This shouldn't happen");
    }

    if (run->global->io.dynfileqCurrent == NULL) {
        run->global->io.dynfileqCurrent = TAILQ_FIRST(&run->global->io.dynfileq);
    } else {
        run->global->io.dynfileqCurrent = TAILQ_NEXT(run->global->io.dynfileqCurrent, pointers);
    }
    if (run->global->io.dynfileqCurrent == NULL) {
        return false;
    }

    LOG_I("Testing file '%s', coverage: %" PRIu64 "/%" PRIu64 "/%" PRIu64,
        run->global->io.dynfileqCurrent->path, run->global->io.dynfileqCurrent->cov1l,
        run->global->io.dynfileqCurrent->cov2l, run->global->io.dynfileqCurrent->cov3l);

    input_setSize(run, run->global->io.dynfileqCurrent->size);
    memcpy(run->dynamicFile, run->global->io.dynfileqCurrent->data,
        run->global->io.dynfileqCurrent->size);
    snprintf(
        run->origFileName, sizeof(run->origFileName), "%s", run->global->io.dynfileqCurrent->path);

    return true;
}

bool input_feedbackMutateFile(run_t* run) {
    int fd =
        files_writeBufToTmpFile(run->global->io.workDir, run->dynamicFile, run->dynamicFileSz, 0);
    if (fd == -1) {
        LOG_E("Couldn't write input file to a temporary buffer");
        return false;
    }
    defer {
        close(fd);
    };

    char fname[PATH_MAX];
    snprintf(fname, sizeof(fname), "/dev/fd/%d", fd);

    const char* const argv[] = {run->global->exe.feedbackMutateCommand, fname, NULL};
    if (subproc_System(run, argv) != 0) {
        LOG_E("Subprocess '%s' returned abnormally", run->global->exe.feedbackMutateCommand);
        return false;
    }
    LOG_D("Subporcess '%s' finished with success", run->global->exe.externalCommand);

    input_setSize(run, run->global->mutate.maxFileSz);
    ssize_t sz = files_readFromFdSeek(fd, run->dynamicFile, run->global->mutate.maxFileSz, 0);
    if (sz == -1) {
        LOG_E("Couldn't read file from fd=%d", fd);
        return false;
    }

    input_setSize(run, (size_t)sz);
    return true;
}

#define TAILQ_FOREACH_HF(var, head, field) \
    for ((var) = TAILQ_FIRST((head)); (var); (var) = TAILQ_NEXT((var), field))

/* Yes, the bubblesort :) */
void input_sortDynamicInput(honggfuzz_t* hfuzz) {
    LOG_I("Sorting %zu dynamic entries by coverage", hfuzz->io.dynfileqCnt);

    for (size_t i = 0; i < hfuzz->io.dynfileqCnt; i++) {
        struct dynfile_t* item = NULL;
        TAILQ_FOREACH_HF(item, &hfuzz->io.dynfileq, pointers) {
            struct dynfile_t* itemnext = TAILQ_NEXT(item, pointers);
            if (itemnext == NULL) {
                continue;
            }
            if (itemnext->cov1l < item->cov1l) {
                continue;
            }
            if (itemnext->cov1l == item->cov1l && itemnext->cov2l < item->cov2l) {
                continue;
            }
            if (itemnext->cov1l == item->cov1l && itemnext->cov2l == item->cov2l &&
                itemnext->cov3l < item->cov3l) {
                continue;
            }

            TAILQ_REMOVE(&hfuzz->io.dynfileq, itemnext, pointers);
            TAILQ_INSERT_BEFORE(item, itemnext, pointers);

            /* We've swapped items, so rewind item to the itemnext */
            item = itemnext;
        }
    }
}
