/**
 * \file lib/legacy.c
 */

#include "system.h"

#include <rpm/header.h>
#include <rpm/rpmmacro.h>
#include <rpm/rpmstring.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmds.h>

#include "lib/legacy.h"

#include "debug.h"

int _noDirTokens = 0;

static int dncmp(const void * a, const void * b)
{
    const char *const * first = a;
    const char *const * second = b;
    return strcmp(*first, *second);
}

void compressFilelist(Header h)
{
    struct rpmtd_s fileNames;
    char ** dirNames;
    const char ** baseNames;
    uint32_t * dirIndexes;
    rpm_count_t count;
    int xx, i;
    int dirIndex = -1;

    /*
     * This assumes the file list is already sorted, and begins with a
     * single '/'. That assumption isn't critical, but it makes things go
     * a bit faster.
     */

    if (headerIsEntry(h, RPMTAG_DIRNAMES)) {
	xx = headerDel(h, RPMTAG_OLDFILENAMES);
	return;		/* Already converted. */
    }

    if (!headerGet(h, RPMTAG_OLDFILENAMES, &fileNames, HEADERGET_MINMEM)) 
	return;
    count = rpmtdCount(&fileNames);
    if (count < 1) 
	return;

    dirNames = xmalloc(sizeof(*dirNames) * count);	/* worst case */
    baseNames = xmalloc(sizeof(*dirNames) * count);
    dirIndexes = xmalloc(sizeof(*dirIndexes) * count);

    if (headerIsSource(h)) {
	/* HACK. Source RPM, so just do things differently */
	dirIndex = 0;
	dirNames[dirIndex] = xstrdup("");
	while ((i = rpmtdNext(&fileNames)) >= 0) {
	    dirIndexes[i] = dirIndex;
	    baseNames[i] = rpmtdGetString(&fileNames);
	}
	goto exit;
    }

    while ((i = rpmtdNext(&fileNames)) >= 0) {
	char ** needle;
	char savechar;
	char * baseName;
	size_t len;
	const char *filename = rpmtdGetString(&fileNames);

	if (filename == NULL)	/* XXX can't happen */
	    continue;
	baseName = strrchr(filename, '/') + 1;
	len = baseName - filename;
	needle = dirNames;
	savechar = *baseName;
	*baseName = '\0';
	if (dirIndex < 0 ||
	    (needle = bsearch(filename, dirNames, dirIndex + 1, sizeof(dirNames[0]), dncmp)) == NULL) {
	    char *s = xmalloc(len + 1);
	    rstrlcpy(s, filename, len + 1);
	    dirIndexes[i] = ++dirIndex;
	    dirNames[dirIndex] = s;
	} else
	    dirIndexes[i] = needle - dirNames;

	*baseName = savechar;
	baseNames[i] = baseName;
    }

exit:
    if (count > 0) {
	struct rpmtd_s td;
	if (rpmtdFromUint32(&td, RPMTAG_DIRINDEXES, dirIndexes, count))
	    headerPut(h, &td, HEADERPUT_DEFAULT);
	if (rpmtdFromStringArray(&td, RPMTAG_BASENAMES, baseNames, count))
	    headerPut(h, &td, HEADERPUT_DEFAULT);
	if (rpmtdFromStringArray(&td, RPMTAG_DIRNAMES, 
				 (const char **) dirNames, dirIndex + 1))
	    headerPut(h, &td, HEADERPUT_DEFAULT);
    }

    rpmtdFreeData(&fileNames);
    for (i = 0; i <= dirIndex; i++) {
	free(dirNames[i]);
    }
    free(dirNames);
    free(baseNames);
    free(dirIndexes);

    xx = headerDel(h, RPMTAG_OLDFILENAMES);
}

void expandFilelist(Header h)
{
    struct rpmtd_s filenames;

    if (!headerIsEntry(h, RPMTAG_OLDFILENAMES)) {
	(void) headerGet(h, RPMTAG_FILENAMES, &filenames, HEADERGET_EXT);
	if (rpmtdCount(&filenames) < 1)
	    return;
	rpmtdSetTag(&filenames, RPMTAG_OLDFILENAMES);
	headerPut(h, &filenames, HEADERPUT_DEFAULT);
	rpmtdFreeData(&filenames);
    }

    (void) headerDel(h, RPMTAG_DIRNAMES);
    (void) headerDel(h, RPMTAG_BASENAMES);
    (void) headerDel(h, RPMTAG_DIRINDEXES);
}

/*
 * Up to rpm 3.0.4, packages implicitly provided their own name-version-release.
 * Retrofit an explicit "Provides: name = epoch:version-release.
 */
static void providePackageNVR(Header h)
{
    const char *name;
    char *pEVR;
    rpmsenseFlags pFlags = RPMSENSE_EQUAL;
    int bingo = 1;
    struct rpmtd_s pnames;
    rpmds hds, nvrds;

    /* Generate provides for this package name-version-release. */
    pEVR = headerGetEVR(h, &name);
    if (!(name && pEVR))
	return;

    /*
     * Rpm prior to 3.0.3 does not have versioned provides.
     * If no provides at all are available, we can just add.
     */
    if (!headerGet(h, RPMTAG_PROVIDENAME, &pnames, HEADERGET_MINMEM)) {
	goto exit;
    }

    /*
     * Otherwise, fill in entries on legacy packages.
     */
    if (!headerIsEntry(h, RPMTAG_PROVIDEVERSION)) {
	while (rpmtdNext(&pnames) >= 0) {
	    const char * vdummy = "";
	    rpmsenseFlags fdummy = RPMSENSE_ANY;
	    struct rpmtd_s td;

	    if (rpmtdFromStringArray(&td, RPMTAG_PROVIDEVERSION, &vdummy, 1))
		headerPut(h, &td, HEADERPUT_APPEND);
	    if (rpmtdFromUint32(&td, RPMTAG_PROVIDEFLAGS, &fdummy, 1))
		headerPut(h, &td, HEADERPUT_APPEND);
	}
	goto exit;
    }

    /* see if we already have this provide */
    hds = rpmdsNew(h, RPMTAG_PROVIDENAME, 1);
    nvrds = rpmdsSingle(RPMTAG_PROVIDENAME, name, pEVR, pFlags);
    if (rpmdsFind(hds, nvrds) >= 0) {
	bingo = 0;
    }
    rpmdsFree(hds);
    rpmdsFree(nvrds);
    

exit:
    if (bingo) {
	struct rpmtd_s td;
	const char *evr = pEVR;
	if (rpmtdFromStringArray(&td, RPMTAG_PROVIDENAME, &name, 1))
	    headerPut(h, &td, HEADERPUT_APPEND);
	if (rpmtdFromUint32(&td, RPMTAG_PROVIDEFLAGS, &pFlags, 1))
	    headerPut(h, &td, HEADERPUT_APPEND);
	if (rpmtdFromStringArray(&td, RPMTAG_PROVIDEVERSION, &evr, 1))
	    headerPut(h, &td, HEADERPUT_APPEND);
    }
    rpmtdFreeData(&pnames);
    free(pEVR);
}

void legacyRetrofit(Header h)
{
    struct rpmtd_s dprefix;

    /*
     * We don't use these entries (and rpm >= 2 never has) and they are
     * pretty misleading. Let's just get rid of them so they don't confuse
     * anyone.
     */
    if (headerIsEntry(h, RPMTAG_FILEUSERNAME))
	(void) headerDel(h, RPMTAG_FILEUIDS);
    if (headerIsEntry(h, RPMTAG_FILEGROUPNAME))
	(void) headerDel(h, RPMTAG_FILEGIDS);

    /*
     * We switched the way we do relocatable packages. We fix some of
     * it up here, though the install code still has to be a bit 
     * careful. This fixup makes queries give the new values though,
     * which is quite handy.
     */
    if (headerGet(h, RPMTAG_DEFAULTPREFIX, &dprefix, HEADERGET_MINMEM)) {
	const char *prefix = rpmtdGetString(&dprefix);
	char * nprefix = stripTrailingChar(xstrdup(prefix), '/');
	struct rpmtd_s td;
	
	if (rpmtdFromStringArray(&td, RPMTAG_PREFIXES, 
					(const char **) &nprefix, 1))
	    headerPut(h, &td, HEADERPUT_DEFAULT);
	free(nprefix);
	rpmtdFreeData(&dprefix);
    }

    /*
     * The file list was moved to a more compressed format which not
     * only saves memory (nice), but gives fingerprinting a nice, fat
     * speed boost (very nice). Go ahead and convert old headers to
     * the new style (this is a noop for new headers).
     */
     compressFilelist(h);

    /* XXX binary rpms always have RPMTAG_SOURCERPM, source rpms do not */
    if (headerIsSource(h)) {
	uint32_t one = 1;
	struct rpmtd_s td;

	if (!headerIsEntry(h, RPMTAG_SOURCEPACKAGE))
	    if (rpmtdFromUint32(&td, RPMTAG_SOURCEPACKAGE, &one, 1))
		headerPut(h, &td, HEADERPUT_DEFAULT);
    } else {
	/* Retrofit "Provide: name = EVR" for binary packages. */
	providePackageNVR(h);
    }
}
