// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2015-2020, Intel Corporation */

/*
 * transaction.c -- unit test for pool conversion
 *
 * This test has dual purpose - to create an old-format pool with the _create
 * functions and to verify if the conversion happened correctly.
 *
 * The creation should happen while linked with the old library version and
 * the verify step should be run with the new one.
 */

#include <stdio.h>
#include <stdlib.h>
#include "libpmemobj.h"

POBJ_LAYOUT_BEGIN(convert);
POBJ_LAYOUT_ROOT(convert, struct root);
POBJ_LAYOUT_TOID(convert, struct foo);
POBJ_LAYOUT_TOID(convert, struct bar);
POBJ_LAYOUT_END(convert);

#define SMALL_ALLOC (64)
#define BIG_ALLOC (1024 * 200) /* just big enough to be a huge allocation */

struct bar {
	char value[BIG_ALLOC];
};

struct foo {
	unsigned char value[SMALL_ALLOC];
};

#define TEST_VALUE 5
#define TEST_NVALUES 10
#define TEST_RECURSION_NUM 5

struct root {
	TOID(struct foo) foo;
	TOID(struct bar) bar;
	int value[TEST_NVALUES];
};

/*
 * A global variable used to trigger a breakpoint in the gdb in order to stop
 * execution of the test after it was used. It's used to simulate a crash in the
 * tx_commit process.
 */
static int trap = 0;

enum operation {
	ADD,
	DRW,
	SET
};

/*
 * A macro that recursively create a nested transactions and save whole object
 * or specific FIELD in the undo log.
 */
#define TEST_GEN(type, x)\
static void \
type ## _tx(PMEMobjpool *pop, TOID(struct type) var, int array_size,\
	int recursion, enum operation oper)\
{\
	--recursion;\
\
	TX_BEGIN(pop) {\
		if (oper == ADD) {\
			TX_ADD(var);\
			oper = DRW;\
		}\
\
		if (recursion >= 1)\
			TEST_CALL(type, pop, var, array_size, recursion,\
				oper);\
\
		for (int i = 0; i < array_size; ++i) {\
			if (oper == SET)\
				TX_SET(var, value[i], (x)(TEST_VALUE +\
					D_RO(var)->value[i]));\
			else if (oper == DRW)\
				D_RW(var)->value[i] = (x)(TEST_VALUE +\
					D_RO(var)->value[i]);\
		}\
	} TX_END\
}

#define TEST_CALL(type, pop, var, array_size, recursion, oper)\
	type ## _tx(pop, var, array_size, recursion, oper)

TEST_GEN(foo, unsigned char);
TEST_GEN(bar, char);
TEST_GEN(root, int);

/*
 * sc0_create -- single large set undo
 */
static void
sc0_create(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	trap = 1;
	TX_BEGIN(pop) {
		TX_ADD(rt);
		D_RW(rt)->value[0] = TEST_VALUE;
	} TX_END
}

static void
sc0_verify_abort(PMEMobjpool *pop)
{
	if (pmemobj_root_size(pop) != sizeof(struct root))
		exit(1);
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	if (D_RW(rt)->value[0] != 0)
		exit(2);
}

static void
sc0_verify_commit(PMEMobjpool *pop)
{
	if (pmemobj_root_size(pop) != sizeof(struct root))
		exit(3);
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	if (D_RW(rt)->value[0] != TEST_VALUE)
		exit(4);
}

/*
 * sc1_create -- single small set undo
 */
static void
sc1_create(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	POBJ_ZALLOC(pop, &D_RW(rt)->foo, struct foo,
		sizeof(struct foo));
	trap = 1;

	TX_BEGIN(pop) {
		TX_ADD(D_RW(rt)->foo);
		D_RW(D_RW(rt)->foo)->value[0] = TEST_VALUE;
	} TX_END
}

static void
sc1_verify_abort(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	if (D_RW(D_RW(rt)->foo)->value[0] != 0)
		exit(5);
}

static void
sc1_verify_commit(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	if (D_RW(D_RW(rt)->foo)->value[0] != TEST_VALUE)
		exit(6);
}

/*
 * sc2_create -- multiply changes in large set undo (TX_ADD)
 */
static void
sc2_create(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	TX_BEGIN(pop) {
		TEST_CALL(root, pop, rt, TEST_NVALUES, TEST_RECURSION_NUM, ADD);
		trap = 1;
		TEST_CALL(root, pop, rt, TEST_NVALUES, TEST_RECURSION_NUM, ADD);
	} TX_END
}

static void
sc2_verify_abort(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	for (int i = 0; i < TEST_NVALUES; ++i)
		if (D_RW(rt)->value[i] != 0)
			exit(7);
}

static void
sc2_verify_commit(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	for (int i = 0; i < TEST_NVALUES; ++i)
		if (D_RW(rt)->value[i] != 2 * TEST_RECURSION_NUM * TEST_VALUE)
			exit(8);
}

/*
 * sc3_create -- multiply changes in small set undo (TX_SET)
 */
static void
sc3_create(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	POBJ_ZALLOC(pop, &D_RW(rt)->bar, struct bar,
		sizeof(struct bar));

	TX_BEGIN(pop) {
		TEST_CALL(bar, pop, D_RW(rt)->bar, BIG_ALLOC,
			TEST_RECURSION_NUM, SET);
		trap = 1;
		TEST_CALL(bar, pop, D_RW(rt)->bar, BIG_ALLOC,
			TEST_RECURSION_NUM, SET);
	} TX_END
}

static void
sc3_verify_abort(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	for (int i = 0; i < TEST_NVALUES; ++i)
		if (D_RW(D_RW(rt)->bar)->value[i] != 0)
			exit(9);
}

static void
sc3_verify_commit(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	for (int i = 0; i < TEST_NVALUES; ++i)
		if (D_RW(D_RW(rt)->bar)->value[i] !=\
			2 * TEST_RECURSION_NUM * TEST_VALUE)
			exit(10);
}

/*
 * sc4_create -- multiply changes in small set undo (TX_ADD)
 */
static void
sc4_create(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	POBJ_ZALLOC(pop, &D_RW(rt)->foo, struct foo,
		sizeof(struct foo));

	TX_BEGIN(pop) {
		TEST_CALL(foo, pop, D_RW(rt)->foo, SMALL_ALLOC,
			TEST_RECURSION_NUM, ADD);
		trap = 1;
		TEST_CALL(foo, pop, D_RW(rt)->foo, SMALL_ALLOC,
			TEST_RECURSION_NUM, ADD);
	} TX_END
}

static void
sc4_verify_abort(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	for (int i = 0; i < SMALL_ALLOC; ++i)
		if (D_RW(D_RW(rt)->foo)->value[i] != 0)
			exit(11);
}

static void
sc4_verify_commit(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	for (int i = 0; i < SMALL_ALLOC; ++i)
		if (D_RW(D_RW(rt)->foo)->value[i] !=\
			2 * TEST_RECURSION_NUM * TEST_VALUE)
			exit(12);
}

/*
 * sc5_create -- multiply changes in small set undo (TX_SET)
 */
static void
sc5_create(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	POBJ_ZALLOC(pop, &D_RW(rt)->foo, struct foo,
		sizeof(struct foo));

	TX_BEGIN(pop) {
		TEST_CALL(foo, pop, D_RW(rt)->foo, SMALL_ALLOC,
			TEST_RECURSION_NUM, SET);
		trap = 1;
		TEST_CALL(foo, pop, D_RW(rt)->foo, SMALL_ALLOC,
			TEST_RECURSION_NUM, SET);
	} TX_END
}

static void
sc5_verify_abort(PMEMobjpool *pop)
{
	sc4_verify_abort(pop);
}

static void
sc5_verify_commit(PMEMobjpool *pop)
{
	sc4_verify_commit(pop);
}

/*
 * sc6_create -- free undo
 */
static void
sc6_create(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	TX_BEGIN(pop) {
		TX_SET(rt, foo, TX_NEW(struct foo));
		TX_SET(rt, bar, TX_NEW(struct bar));
	} TX_ONABORT {
		exit(0);
	} TX_END

	trap = 1;

	TX_BEGIN(pop) {
		TX_FREE(D_RO(rt)->foo);
		TX_FREE(D_RO(rt)->bar);
	} TX_END
}

static void
sc6_verify_abort(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	TX_BEGIN(pop) {
		/*
		 * If the free undo log didn't get unrolled then the next
		 * free would fail due to the object being already freed.
		 */
		TX_FREE(D_RO(rt)->foo);
		TX_FREE(D_RO(rt)->bar);
	} TX_ONABORT {
		exit(0);
	} TX_END
}

static void
sc6_verify_commit(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	TOID(struct foo) f;
	POBJ_FOREACH_TYPE(pop, f) {
		if (!TOID_EQUALS(f, D_RO(rt)->foo))
			exit(13);
	}
	TOID(struct bar) b;
	POBJ_FOREACH_TYPE(pop, b) {
		if (!TOID_EQUALS(b, D_RO(rt)->bar))
			exit(14);
	}
}

/*
 * sc7_create -- multiple small and large set undo
 */
static void
sc7_create(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);
	POBJ_ZALLOC(pop, &D_RW(rt)->bar, struct bar,
		sizeof(struct bar));
	POBJ_ZALLOC(pop, &D_RW(rt)->foo, struct foo,
		sizeof(struct foo));

	TX_BEGIN(pop) {
		TEST_CALL(foo, pop, D_RW(rt)->foo, SMALL_ALLOC,
			TEST_RECURSION_NUM, SET);
		TEST_CALL(bar, pop, D_RW(rt)->bar, BIG_ALLOC,
			TEST_RECURSION_NUM, SET);
		TEST_CALL(root, pop, rt, TEST_NVALUES, TEST_RECURSION_NUM, SET);
		trap = 1;
		TEST_CALL(foo, pop, D_RW(rt)->foo, SMALL_ALLOC,
			TEST_RECURSION_NUM, ADD);
		TEST_CALL(bar, pop, D_RW(rt)->bar, BIG_ALLOC,
			TEST_RECURSION_NUM, ADD);
		TEST_CALL(root, pop, rt, TEST_NVALUES, TEST_RECURSION_NUM, ADD);
	} TX_END
}

static void
sc7_verify_abort(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	for (int i = 0; i < SMALL_ALLOC; ++i)
		if (D_RW(D_RW(rt)->foo)->value[i] != 0)
			exit(18);
	for (int i = 0; i < BIG_ALLOC; ++i)
		if (D_RW(D_RW(rt)->bar)->value[i] != 0)
			exit(19);
	for (int i = 0; i < TEST_NVALUES; ++i)
		if (D_RW(rt)->value[i] != 0)
			exit(20);
}

static void
sc7_verify_commit(PMEMobjpool *pop)
{
	TOID(struct root) rt = POBJ_ROOT(pop, struct root);

	for (int i = 0; i < SMALL_ALLOC; ++i)
		if (D_RW(D_RW(rt)->foo)->value[i] != 2 * TEST_RECURSION_NUM *\
			TEST_VALUE)
			exit(21);
	for (int i = 0; i < BIG_ALLOC; ++i)
		if (D_RW(D_RW(rt)->bar)->value[i] != 2 * TEST_RECURSION_NUM *\
			  TEST_VALUE)
			exit(22);
	for (int i = 0; i < TEST_NVALUES; ++i)
		if (D_RW(rt)->value[i] != 2 * TEST_RECURSION_NUM * TEST_VALUE)
			exit(23);
}

/*
 * sc8_create -- small alloc undo
 */
static void
sc8_create(PMEMobjpool *pop)
{
	/* allocate until OOM and count allocs */
	int nallocs = 0;

	TX_BEGIN(pop) {
		for (;;) {
			(void) TX_NEW(struct foo);
			nallocs++;
		}
	} TX_END

	trap = 1;
	/* allocate all possible objects */
	TX_BEGIN(pop) {
		for (int i = 0; i < nallocs; ++i) {
			(void) TX_NEW(struct foo);
		}
	} TX_ONABORT {
		exit(0);
	} TX_END
}

static void
sc8_verify_abort(PMEMobjpool *pop)
{
	int nallocs = 0;

	TOID(struct foo) f;
	POBJ_FOREACH_TYPE(pop, f) {
		nallocs++;
	}

	if (nallocs != 0)
		exit(15);
}

static void
sc8_verify_commit(PMEMobjpool *pop)
{
	int nallocs = 0;

	TOID(struct foo) f;
	POBJ_FOREACH_TYPE(pop, f) {
		nallocs++;
	}

	if (nallocs == 0)
		exit(16);
}

/*
 * sc9_create -- large alloc undo
 */
static void
sc9_create(PMEMobjpool *pop)
{
	/* allocate until OOM and count allocs */
	int nallocs = 0;

	TX_BEGIN(pop) {
		for (;;) {
			(void) TX_NEW(struct bar);
			nallocs++;
		}
	} TX_END

	trap = 1;
	/* allocate all possible objects */
	TX_BEGIN(pop) {
		for (int i = 0; i < nallocs; ++i) {
			(void) TX_NEW(struct bar);
		}
	} TX_ONABORT {
		exit(0);
	} TX_END
}

static void
sc9_verify_abort(PMEMobjpool *pop)
{
	TX_BEGIN(pop) {
		TOID(struct bar) f = TX_NEW(struct bar);
		(void) f;
	} TX_ONABORT {
		exit(0);
	} TX_END
}

static void
sc9_verify_commit(PMEMobjpool *pop)
{
	int nallocs = 0;

	TOID(struct bar) f;
	POBJ_FOREACH_TYPE(pop, f) {
		nallocs++;
	}

	if (nallocs == 0)
		exit(17);
}

typedef void (*scenario_func)(PMEMobjpool *pop);

static struct {
	scenario_func create;
	scenario_func verify_abort;
	scenario_func verify_commit;
} scenarios[] = {
	{sc0_create, sc0_verify_abort, sc0_verify_commit},
	{sc1_create, sc1_verify_abort, sc1_verify_commit},
	{sc2_create, sc2_verify_abort, sc2_verify_commit},
	{sc3_create, sc3_verify_abort, sc3_verify_commit},
	{sc4_create, sc4_verify_abort, sc4_verify_commit},
	{sc5_create, sc5_verify_abort, sc5_verify_commit},
	{sc6_create, sc6_verify_abort, sc6_verify_commit},
	{sc7_create, sc7_verify_abort, sc7_verify_commit},
	{sc8_create, sc8_verify_abort, sc8_verify_commit},
	{sc9_create, sc9_verify_abort, sc9_verify_commit},
};

int
main(int argc, char *argv[])
{
	if (argc != 4)
		printf("usage: %s file [c|va|vc] scenario", argv[0]);

#ifdef _WIN32
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
#else
	const char *path = argv[1];
#endif

	int prepare_pool = argv[2][0] == 'c';
	int verify_abort = argv[2][1] == 'a';
	int sc = atoi(argv[3]);

	PMEMobjpool *pop = NULL;
#ifdef _WIN32
	if ((pop = pmemobj_openW(wargv[1], NULL)) == NULL) {
			printf("failed to open pool\n");
			exit(24);
	}
#else
	if ((pop = pmemobj_open(path, NULL)) == NULL) {
			printf("failed to open pool\n");
			exit(24);
	}
#endif

	if (prepare_pool)
		scenarios[sc].create(pop);
	else {
		if (verify_abort)
			scenarios[sc].verify_abort(pop);
		else
			scenarios[sc].verify_commit(pop);
	}

	pmemobj_close(pop);

	return 0;
}