/* Jim - A small embeddable Tcl interpreter
 * Copyright 2005 Salvatore Sanfilippo <antirez@invece.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * A copy of the license is also included in the source distribution
 * of Jim, as a TXT file name called LICENSE.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <time.h>

#include "jim.h"

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#endif

/* -----------------------------------------------------------------------------
 * Global variables
 * ---------------------------------------------------------------------------*/

/* A shared empty string for the objects string representation.
 * Jim_InvalidateStringRep knows about it and don't try to free. */
char *JimEmptyStringRep = "";

/* -----------------------------------------------------------------------------
 * Required prototypes of not exported functions
 * ---------------------------------------------------------------------------*/
static void Jim_ChangeCallFrameId(Jim_Interp *interp, Jim_CallFrame *cf);
static void Jim_FreeCallFrame(Jim_Interp *interp, Jim_CallFrame *cf);
static void Jim_RegisterCoreApi(Jim_Interp *interp);

/* -----------------------------------------------------------------------------
 * Utility functions
 * ---------------------------------------------------------------------------*/

/*
 * Convert a string to a jim_wide INTEGER.
 * This function originates from BSD.
 *
 * Ignores `locale' stuff.  Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 */
#ifdef HAVE_LONG_LONG
static jim_wide Jim_Strtoll(const char *nptr, char **endptr, register int base)
{
	register const char *s;
	register unsigned jim_wide acc;
	register unsigned char c;
	register unsigned jim_wide qbase, cutoff;
	register int neg, any, cutlim;

	/*
	 * Skip white space and pick up leading +/- sign if any.
	 * If base is 0, allow 0x for hex and 0 for octal, else
	 * assume decimal; if base is already 16, allow 0x.
	 */
	s = nptr;
	do {
		c = *s++;
	} while (isspace(c));
	if (c == '-') {
		neg = 1;
		c = *s++;
	} else {
		neg = 0;
		if (c == '+')
			c = *s++;
	}
	if ((base == 0 || base == 16) &&
	    c == '0' && (*s == 'x' || *s == 'X')) {
		c = s[1];
		s += 2;
		base = 16;
	}
	if (base == 0)
		base = c == '0' ? 8 : 10;

	/*
	 * Compute the cutoff value between legal numbers and illegal
	 * numbers.  That is the largest legal value, divided by the
	 * base.  An input number that is greater than this value, if
	 * followed by a legal input character, is too big.  One that
	 * is equal to this value may be valid or not; the limit
	 * between valid and invalid numbers is then based on the last
	 * digit.  For instance, if the range for quads is
	 * [-9223372036854775808..9223372036854775807] and the input base
	 * is 10, cutoff will be set to 922337203685477580 and cutlim to
	 * either 7 (neg==0) or 8 (neg==1), meaning that if we have
	 * accumulated a value > 922337203685477580, or equal but the
	 * next digit is > 7 (or 8), the number is too big, and we will
	 * return a range error.
	 *
	 * Set any if any `digits' consumed; make it negative to indicate
	 * overflow.
	 */
	qbase = (unsigned)base;
	cutoff = neg ? (unsigned jim_wide)-(LLONG_MIN + LLONG_MAX) + LLONG_MAX
	    : LLONG_MAX;
	cutlim = (int)(cutoff % qbase);
	cutoff /= qbase;
	for (acc = 0, any = 0;; c = *s++) {
		if (!isascii(c))
			break;
		if (isdigit(c))
			c -= '0';
		else if (isalpha(c))
			c -= isupper(c) ? 'A' - 10 : 'a' - 10;
		else
			break;
		if (c >= base)
			break;
		if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
			any = -1;
		else {
			any = 1;
			acc *= qbase;
			acc += c;
		}
	}
	if (any < 0) {
		acc = neg ? LLONG_MIN : LLONG_MAX;
		errno = ERANGE;
	} else if (neg)
		acc = -acc;
	if (endptr != 0)
		*endptr = (char *)(any ? s - 1 : nptr);
	return (acc);
}
#endif

/* Glob-style pattern matching. */
int Jim_StringMatch(char *pattern, char *string, int nocase)
{
	while(pattern[0]) {
		switch(pattern[0]) {
		case '*':
			while (pattern[1] == '*')
				pattern++;
			if (pattern[1] == '\0')
				return 1; /* match */
			while(string[0]) {
				if (Jim_StringMatch(pattern+1, string, nocase))
					return 1;
				string++; /* match */
			}
			return 0; /* no match */
			break;
		case '?':
			string++;
			break;
		case '[':
		{
			int not, match;

			pattern++;
			not = pattern[0] == '^';
			if (not) pattern++;
			match = 0;
			while(1) {
				if (pattern[0] == '\\') {
					pattern++;
					if (pattern[0] == string[0])
						match = 1;
				} else if (pattern[0] == ']') {
					break;
				} else if (pattern[0] == '\0') {
					pattern--;
					break;
				} else if (pattern[1] == '-' &&
					   pattern[2] != '\0') {
					int start = pattern[0];
					int end = pattern[2];
					int c = string[0];
					if (nocase) {
						start = tolower(start);
						end = tolower(end);
						c = tolower(c);
					}
					pattern += 2;
					if (c >= start && c <= end)
						match = 1;
				} else {
					if (!nocase) {
						if (pattern[0] == string[0])
							match = 1;
					} else {
						if (tolower((int)pattern[0]) == 
						    tolower((int)string[0]))
							match = 1;
					}
				}
				pattern++;
			}
			if (not)
				match = !match;
			if (!match)
				return 0; /* no match */
			string++;
			break;
		}
		case '\\':
			pattern++;
			/* fall through */
		default:
			if (!nocase) {
				if (pattern[0] != string[0])
					return 0; /* no match */
			} else {
				if (tolower(pattern[0]) !=
				    tolower(string[0]))
					return 0; /* no match */
			}
			string++;
			break;
		}
		pattern++;
		if (string[0] == '\0')
			break;
	}
	if (pattern[0] == '\0' &&
			string[0] == '\0')
		return 1;
	return 0;
}

int testGlobMatching(void)
{
	char buf[1024];
	char *str = "hello worldo";
	
	printf("string: %s\n", str);
	while(1) {
		int len;

		printf("pattern> ");
		if (fgets(buf, 1024, stdin) == NULL) return 0;
		len = strlen(buf);
		if (len && buf[len-1] == '\n') {
			buf[len-1] = '\0';
		}
		printf("%d\n", Jim_StringMatch(buf, str, 0));
	}
	return 0;
}

int Jim_WideToString(char *buf, jim_wide wideValue)
{
#ifdef HAVE_LONG_LONG
	return sprintf(buf, "%" JIM_LL_MODIFIER, wideValue);
#else
	return sprintf(buf, "%ld", wideValue);
#endif
}

int Jim_StringToWide(char *str, jim_wide *widePtr, int base)
{
	char *endptr;

#ifdef HAVE_LONG_LONG
	*widePtr = Jim_Strtoll(str, &endptr, base);
#else
	*widePtr = strtol(str, &endptr, base);
#endif
	if (str[0] == '\0' || endptr[0] != '\0')
		return JIM_ERR;
	return JIM_OK;
}

/* The string representation of references has two features in order
 * to make the GC faster. The first is that every reference starts
 * with a non common character '~', in order to make the string matching
 * fater. The second is that the reference string rep his 32 characters
 * in length, this allows to avoid to check every object with a string
 * repr < 32, and usually there are many of this objects. */

#define JIM_REFERENCE_SPACE 32

int Jim_WideToReferenceString(char *buf, jim_wide wideValue)
{
#ifdef HAVE_LONG_LONG
	sprintf(buf, "~reference:%020" JIM_LL_MODIFIER ":", wideValue);
#else
	sprintf(buf, "~reference:%020ld:", wideValue);
#endif
	return JIM_REFERENCE_SPACE;
}

int Jim_DoubleToString(char *buf, double doubleValue)
{
	char *s;
	int len;

	len = sprintf(buf, "%.17g", doubleValue);
	s = buf;
	while(*s) {
		if (*s == '.') return len;
		s++;
	}
	s[0] = '.';
	s[1] = '0';
	s[2] = '\0';
	return len+2;
}

int Jim_StringToDouble(char *str, double *doublePtr)
{
	char *endptr;

	*doublePtr = strtod(str, &endptr);
	if (str[0] == '\0' || endptr[0] != '\0')
		return JIM_ERR;
	return JIM_OK;
}

/* -----------------------------------------------------------------------------
 * Special functions
 * ---------------------------------------------------------------------------*/
void Jim_Panic(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "\nJIM INTERPRETER PANIC: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n\n");
	va_end(ap);
#ifdef HAVE_BACKTRACE
	{
		void *array[40];
		int size, i;
		char **strings;

		size = backtrace(array, 40);
		strings = backtrace_symbols(array, size);
		for (i = 0; i < size; i++)
			printf("[backtrace] %s\n", strings[i]);
		printf("[backtrace] Include the above lines and the output\n");
		printf("[backtrace] of 'nm <executable>' in the bug report.\n");
	}
#endif
	abort();
}

/* -----------------------------------------------------------------------------
 * Memory allocation
 * ---------------------------------------------------------------------------*/
static void *Jim_Alloc(int size);
static char *Jim_StrDup(char *s);

void *Jim_Alloc(int size)
{
	void *p = malloc(size);
	if (p == NULL)
		Jim_Panic("Out of memory");
	return p;
}

void *Jim_Realloc(void *ptr, int size)
{
	void *p = realloc(ptr, size);
	if (p == NULL)
		Jim_Panic("Out of memory");
	return p;
}

char *Jim_StrDup(char *s)
{
	int l = strlen(s);
	char *copy = Jim_Alloc(l+1);

	memcpy(copy, s, l+1);
	return copy;
}

char *Jim_StrDupLen(char *s, int l)
{
	char *copy = Jim_Alloc(l+1);
	
	memcpy(copy, s, l+1);
	return copy;
}

#define Jim_Free free

/* -----------------------------------------------------------------------------
 * Time related functions
 * ---------------------------------------------------------------------------*/
/* Returns microseconds of CPU used since start. */
static long Jim_Clock(void)
{
	clock_t clocks = clock();

	return (long)(clocks*(1000000/CLOCKS_PER_SEC));
}

/* -----------------------------------------------------------------------------
 * Hash Tables
 * ---------------------------------------------------------------------------*/

/* -------------------------- private prototypes ---------------------------- */
static int Jim_ExpandHashTableIfNeeded(Jim_HashTable *ht);
static unsigned int Jim_HashTableNextPower(unsigned int size);
static int Jim_InsertHashEntry(Jim_HashTable *ht, void *key);

/* -------------------------- hash functions -------------------------------- */

/* Thomas Wang's 32 bit Mix Function */
unsigned int Jim_IntHashFunction(unsigned int key)
{
	key += ~(key << 15);
	key ^=  (key >> 10);
	key +=  (key << 3);
	key ^=  (key >> 6);
	key += ~(key << 11);
	key ^=  (key >> 16);
	return key;
}

/* Identity hash function for integer keys */
unsigned int Jim_IdentityHashFunction(unsigned int key)
{
	return key;
}

/* The djb hash function, that's under public domain */
unsigned int Jim_DjbHashFunction(unsigned char *buf, int len)
{
	unsigned int h = 5381;
	while(len--)
		h = (h + (h << 5)) ^ *buf++;
	return h;
}

unsigned int Jim_RightDbjHashFunction(unsigned char *buf, int len)
{
	unsigned int h = 5381;
	buf += len-1;
	while(len--)
		h = (h + (h << 5)) ^ *buf--;
	return h;
}

/* Another simple hash function mixing bit rotation and addition */
#define ROT32R(x,n) (((x)>>(n))|((x)<<(32-(n))))
unsigned int Jim_RotHashFunction(unsigned char *buf, int len)
{
	unsigned int h = 0;
	while(len--) {
		h = h + *buf++;
		h = ROT32R(h, 3);
	}
	return h;
}

unsigned int Jim_RightRotHashFunction(unsigned char *buf, int len)
{
	unsigned int h = 0;
	buf += len-1;
	while(len--) {
		h = h + *buf--;
		h = ROT32R(h, 3);
	}
	return h;
}

/* ----------------------------- API implementation ------------------------- */
/* reset an hashtable already initialized with ht_init().
 * NOTE: This function should only called by ht_destroy(). */
static void Jim_ResetHashTable(Jim_HashTable *ht)
{
	ht->table = NULL;
	ht->size = 0;
	ht->sizemask = 0;
	ht->used = 0;
	ht->collisions = 0;
}

/* Initialize the hash table */
int Jim_InitHashTable(Jim_HashTable *ht, Jim_HashTableType *type,
		void *privDataPtr)
{
	Jim_ResetHashTable(ht);
	ht->type = type;
	ht->privdata = privDataPtr;
	return JIM_OK;
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USER/BUCKETS ration near to <= 1 */
int Jim_ResizeHashTable(Jim_HashTable *ht)
{
	int minimal = ht->used;

	if (minimal < JIM_HT_INITIAL_SIZE)
		minimal = JIM_HT_INITIAL_SIZE;
	return Jim_ExpandHashTable(ht, minimal);
}

/* Expand or create the hashtable */
int Jim_ExpandHashTable(Jim_HashTable *ht, unsigned int size)
{
	Jim_HashTable n; /* the new hashtable */
	unsigned int realsize = Jim_HashTableNextPower(size), i;

	/* the size is invalid if it is smaller than the number of
	 * elements already inside the hashtable */
	if (ht->used >= size)
		return JIM_ERR;

	Jim_InitHashTable(&n, ht->type, ht->privdata);
	n.size = realsize;
	n.sizemask = realsize-1;
	n.table = Jim_Alloc(realsize*sizeof(Jim_HashEntry*));

	/* Initialize all the pointers to NULL */
	memset(n.table, 0, realsize*sizeof(Jim_HashEntry*));

	/* Copy all the elements from the old to the new table:
	 * note that if the old hash table is empty ht->size is zero,
	 * so Jim_ExpandHashTable just creates an hash table. */
	n.used = ht->used;
	for (i = 0; i < ht->size && ht->used > 0; i++) {
		Jim_HashEntry *he, *nextHe;

		if (ht->table[i] == NULL) continue;
		
		/* For each hash entry on this slot... */
		he = ht->table[i];
		while(he) {
			unsigned int h;

			nextHe = he->next;
			/* Get the new element index */
			h = Jim_HashKey(ht, he->key) & n.sizemask;
			he->next = n.table[h];
			n.table[h] = he;
			ht->used--;
			/* Pass to the next element */
			he = nextHe;
		}
	}
	assert(ht->used == 0);
	Jim_Free(ht->table);

	/* Remap the new hashtable in the old */
	*ht = n;
	return JIM_OK;
}

/* Add an element to the target hash table */
int Jim_AddHashEntry(Jim_HashTable *ht, void *key, void *val)
{
	int index;
	Jim_HashEntry *entry;

	/* Get the index of the new element, or -1 if
	 * the element already exists. */
	if ((index = Jim_InsertHashEntry(ht, key)) == -1)
		return JIM_ERR;

	/* Allocates the memory and stores key */
	entry = Jim_Alloc(sizeof(*entry));
	entry->next = ht->table[index];
	ht->table[index] = entry;

	/* Set the hash entry fields. */
	Jim_SetHashKey(ht, entry, key);
	Jim_SetHashVal(ht, entry, val);
	ht->used++;
	return JIM_OK;
}

/* Add an element, discarding the old if the key already exists */
int Jim_ReplaceHashEntry(Jim_HashTable *ht, void *key, void *val)
{
	Jim_HashEntry *entry;

	/* Try to add the element. If the key
	 * does not exists Jim_AddHashEntry will suceed. */
	if (Jim_AddHashEntry(ht, key, val) == JIM_OK)
		return JIM_OK;
	/* It already exists, get the entry */
	entry = Jim_FindHashEntry(ht, key);
	/* Free the old value and set the new one */
	Jim_FreeEntryVal(ht, entry);
	Jim_SetHashVal(ht, entry, val);
	return JIM_OK;
}

/* Search and remove an element */
int Jim_DeleteHashEntry(Jim_HashTable *ht, void *key)
{
	unsigned int h;
	Jim_HashEntry *he, *prevHe;

	if (ht->size == 0)
		return JIM_ERR;
	h = Jim_HashKey(ht, key) & ht->sizemask;
	he = ht->table[h];

	prevHe = NULL;
	while(he) {
		if (Jim_CompareHashKeys(ht, key, he->key)) {
			/* Unlink the element from the list */
			if (prevHe)
				prevHe->next = he->next;
			else
				ht->table[h] = he->next;
			Jim_FreeEntryKey(ht, he);
			Jim_FreeEntryVal(ht, he);
			Jim_Free(he);
			ht->used--;
			return JIM_OK;
		}
		prevHe = he;
		he = he->next;
	}
	return JIM_ERR; /* not found */
}

/* Destroy an entire hash table */
int Jim_FreeHashTable(Jim_HashTable *ht)
{
	unsigned int i;

	/* Free all the elements */
	for (i = 0; i < ht->size && ht->used > 0; i++) {
		Jim_HashEntry *he, *nextHe;

		if ((he = ht->table[i]) == NULL) continue;
		while(he) {
			nextHe = he->next;
			Jim_FreeEntryKey(ht, he);
			Jim_FreeEntryVal(ht, he);
			Jim_Free(he);
			ht->used--;
			he = nextHe;
		}
	}
	/* Free the table and the allocated cache structure */
	Jim_Free(ht->table);
	/* Re-initialize the table */
	Jim_ResetHashTable(ht);
	return JIM_OK; /* never fails */
}

Jim_HashEntry *Jim_FindHashEntry(Jim_HashTable *ht, void *key)
{
	Jim_HashEntry *he;
	unsigned int h;

	if (ht->size == 0) return NULL;
	h = Jim_HashKey(ht, key) & ht->sizemask;
	he = ht->table[h];
	while(he) {
		if (Jim_CompareHashKeys(ht, key, he->key))
			return he;
		he = he->next;
	}
	return NULL;
}

Jim_HashTableIterator *Jim_GetHashTableIterator(Jim_HashTable *ht)
{
	Jim_HashTableIterator *iterator = Jim_Alloc(sizeof(*iterator));

	iterator->ht = ht;
	iterator->index = -1;
	iterator->entry = NULL;
	return iterator;
}

Jim_HashEntry *Jim_NextHashEntry(Jim_HashTableIterator *iterator)
{
	while (1) {
		if (iterator->entry == NULL) {
			iterator->index++;
			if (iterator->index >=
					(signed)iterator->ht->size) break;
			iterator->entry = iterator->ht->table[iterator->index];
		} else {
			iterator->entry = iterator->entry->next;
		}
		if (iterator->entry)
			return iterator->entry;
	}
	return NULL;
}

/* ------------------------- private functions ------------------------------ */

/* Expand the hash table if needed */
static int Jim_ExpandHashTableIfNeeded(Jim_HashTable *ht)
{
	/* If the hash table is empty expand it to the intial size,
	 * if the table is "full" dobule its size. */
	if (ht->size == 0)
		return Jim_ExpandHashTable(ht, JIM_HT_INITIAL_SIZE);
	if (ht->size == ht->used)
		return Jim_ExpandHashTable(ht, ht->size*2);
	return JIM_OK;
}

/* Our hash table capability is a power of two */
static unsigned int Jim_HashTableNextPower(unsigned int size)
{
	unsigned int i = 256;

	if (size >= 2147483648U)
		return 2147483648U;
	while(1) {
		if (i >= size)
			return i;
		i *= 2;
	}
}

/* Returns the index of a free slot that can be populated with
 * an hash entry for the given 'key'.
 * If the key already exists, -1 is returned. */
static int Jim_InsertHashEntry(Jim_HashTable *ht, void *key)
{
	unsigned int h;
	Jim_HashEntry *he;

	/* Expand the hashtable if needed */
	if (Jim_ExpandHashTableIfNeeded(ht) == JIM_ERR)
		return -1;
	/* Compute the key hash value */
	h = Jim_HashKey(ht, key) & ht->sizemask;
	/* Search if this slot does not already contain the given key */
	he = ht->table[h];
	while(he) {
		if (Jim_CompareHashKeys(ht, key, he->key))
			return -1;
		he = he->next;
	}
	return h;
}

/* ----------------------- StringCopy Hash Table Type ------------------------*/

unsigned int Jim_StringCopyHT_HashFunction(void *key)
{
	return Jim_DjbHashFunction(key, strlen(key));
}

void *Jim_StringCopyHT_KeyDup(void *privdata, void *key)
{
	int len = strlen(key);
	char *copy = Jim_Alloc(len+1);
	privdata = privdata; /* not used */

	memcpy(copy, key, len);
	copy[len] = '\0';
	return copy;
}

int Jim_StringCopyHT_KeyCompare(void *privdata, void *key1, void *key2)
{
	privdata = privdata; /* not used */

	return strcmp(key1, key2) == 0;
}

void Jim_StringCopyHT_KeyDestructor(void *privdata, void *key)
{
	privdata = privdata; /* not used */

	Jim_Free(key);
}
	
Jim_HashTableType Jim_StringCopyHashTableType = {
	Jim_StringCopyHT_HashFunction,		/* hash function */
	Jim_StringCopyHT_KeyDup,		/* key dup */
	NULL,					/* val dup */
	Jim_StringCopyHT_KeyCompare,		/* key compare */
	Jim_StringCopyHT_KeyDestructor,		/* key destructor */
	NULL					/* val destructor */
};

/* This is like StringCopy but does not auto-duplicate the key.
 * It's used for intepreter's shared strings. */
Jim_HashTableType Jim_SharedStringsHashTableType = {
	Jim_StringCopyHT_HashFunction,		/* hash function */
	NULL,					/* key dup */
	NULL,					/* val dup */
	Jim_StringCopyHT_KeyCompare,		/* key compare */
	Jim_StringCopyHT_KeyDestructor,		/* key destructor */
	NULL					/* val destructor */
};

/* --------------------------- Int Hash Table Type ---------------------------*/
unsigned int Jim_IntHT_HashFunction(void *key)
{
	return Jim_IntHashFunction((unsigned int)key);
}

Jim_HashTableType Jim_IntHashTableType = {
	Jim_IntHT_HashFunction,			/* hash function */
	NULL,					/* key dup */
	NULL,					/* val dup */
	NULL,					/* key compare */
	NULL,					/* key destructor */
	NULL					/* val destructor */
};

/* ---------------------------- Test & Benchmark  ----------------------------*/

int testHashTable(void)
{
	Jim_HashTable t;
	Jim_HashTableIterator *iterator;
	Jim_HashEntry *entry;
	int i;

	Jim_InitHashTable(&t, &Jim_StringCopyHashTableType, NULL);
	Jim_AddHashEntry(&t, "foo", "bar");
	Jim_AddHashEntry(&t, "ciao", "foobar");
	Jim_AddHashEntry(&t, "a", "1");
	Jim_AddHashEntry(&t, "b", "2");
	Jim_AddHashEntry(&t, "c", "3");
	printf("Used: %d, Size: %d\n", t.used, t.size);

	iterator = Jim_GetHashTableIterator(&t);
	while ((entry = Jim_NextHashEntry(iterator))) {
		printf("%s -> %s\n", (char*)entry->key, (char*)entry->val);
	}
	Jim_FreeHashTableIterator(iterator);
	Jim_FreeHashTable(&t);


	Jim_InitHashTable(&t, &Jim_StringCopyHashTableType, NULL);
	for (i = 0; i < 150000; i++) {
		char buf[64];
		sprintf(buf, "%d", i);
		Jim_AddHashEntry(&t, (void*)buf, (void*)i);
	}
	printf("Size: %d\n", Jim_GetHashTableSize(&t));
	printf("Used: %d\n", Jim_GetHashTableUsed(&t));
	printf("Collisions: %d\n", Jim_GetHashTableCollisions(&t));
	Jim_FreeHashTable(&t);
	return 0;
}

/* -----------------------------------------------------------------------------
 * Stack - This is a simple generic stack implementation. It is used for
 * example in the 'expr' expression compiler.
 * ---------------------------------------------------------------------------*/
typedef struct Jim_Stack {
	int len;
	int maxlen;
	void **vector;
} Jim_Stack;

void Jim_InitStack(Jim_Stack *stack)
{
	stack->len = 0;
	stack->maxlen = 0;
	stack->vector = NULL;
}

void Jim_FreeStack(Jim_Stack *stack)
{
	Jim_Free(stack->vector);
}

int Jim_StackLen(Jim_Stack *stack)
{
	return stack->len;
}

void Jim_StackPush(Jim_Stack *stack, void *element) {
	int neededLen = stack->len+1;
	if (neededLen > stack->maxlen) {
		stack->maxlen = neededLen*2;
		stack->vector = Jim_Realloc(stack->vector, sizeof(void*)*stack->maxlen);
	}
	stack->vector[stack->len] = element;
	stack->len++;
}

void *Jim_StackPop(Jim_Stack *stack)
{
	if (stack->len == 0) return NULL;
	stack->len--;
	return stack->vector[stack->len];
}

void *Jim_StackPeek(Jim_Stack *stack)
{
	if (stack->len == 0) return NULL;
	return stack->vector[stack->len-1];
}

void Jim_FreeStackElements(Jim_Stack *stack, void (*freeFunc)(void *ptr))
{
	int i;

	for (i = 0; i < stack->len; i++)
		freeFunc(stack->vector[i]);
}

/* -----------------------------------------------------------------------------
 * Parser
 * ---------------------------------------------------------------------------*/

/* Token types */
#define JIM_TT_NONE -1		/* No token returned */
#define JIM_TT_STR 0		/* simple string */
#define JIM_TT_ESC 1		/* string that needs escape chars conversion */
#define JIM_TT_VAR 2		/* var substitution */
#define JIM_TT_DICTSUGAR 3	/* Syntax sugar for [dict get], $foo(bar) */
#define JIM_TT_CMD 4		/* command substitution */
#define JIM_TT_SEP 5		/* word separator */
#define JIM_TT_EOL 6		/* line separator */

/* Additional token types needed for expressions */
#define JIM_TT_SUBEXPR_START 7
#define JIM_TT_SUBEXPR_END 8
#define JIM_TT_EXPR_NUMBER 9
#define JIM_TT_EXPR_OPERATOR 10

/* Parser states */
#define JIM_PS_DEF 0		/* Default state */
#define JIM_PS_QUOTE 1		/* Inside "" */

/* Parser context structure. The same context is used both to parse
 * Tcl scripts and lists. */
struct JimParserCtx {
	char *prg;	/* Program text */
	char *p;	/* Pointer to the point of the program we are parsing */
	int linenr;	/* Current line number */
	char *tstart;
	char *tend;	/* Returned token is at tstart-tend in 'prg'. */
	int tline;	/* Line number of the returned token */
	int tt;		/* Token type */
	int eof;	/* Non zero if EOF condition is true. */
	int state;	/* Parser state */
	int comment;	/* Non zero if the next chars may be a comment. */
};

#define JimParserEof(c) ((c)->eof)
#define JimParserTstart(c) ((c)->tstart)
#define JimParserTend(c) ((c)->tend)
#define JimParserTtype(c) ((c)->tt)
#define JimParserTline(c) ((c)->tline)

static int JimParseScript(struct JimParserCtx *pc);
static int JimParseSep(struct JimParserCtx *pc);
static int JimParseEol(struct JimParserCtx *pc);
static int JimParseCmd(struct JimParserCtx *pc);
static int JimParseVar(struct JimParserCtx *pc);
static int JimParseBrace(struct JimParserCtx *pc);
static int JimParseStr(struct JimParserCtx *pc);
static int JimParseComment(struct JimParserCtx *pc);
static char *JimParserGetToken(struct JimParserCtx *pc,
		int *lenPtr, int *typePtr, int *linePtr);

/* Initialize a parser context.
 * 'prg' is a pointer to the program text, linenr is the line
 * number of the first line contained in the program. */
void JimParserInit(struct JimParserCtx *pc, char *prg, int linenr)
{
	pc->prg = prg;
	pc->p = prg;
	pc->tstart = NULL;
	pc->tend = NULL;
	pc->tline = 0;
	pc->tt = JIM_TT_NONE;
	pc->eof = 0;
	pc->state = JIM_PS_DEF;
	pc->linenr = linenr;
	pc->comment = 1;
}

int JimParseScript(struct JimParserCtx *pc)
{
	while(1) { /* the while is used to reiterate with continue if needed */
		switch(*(pc->p)) {
		case '\0':
			pc->tstart = pc->tend = pc->p;
			pc->tline = pc->linenr;
			pc->tt = JIM_TT_EOL;
			pc->eof = 1;
			break;
		case '\\':
			if (*(pc->p+1) == '\n')
				return JimParseSep(pc);
			else {
				pc->comment = 0;
				return JimParseStr(pc);
			}
			break;
		case ' ':
		case '\t':
		case '\r':
			if (pc->state == JIM_PS_DEF)
				return JimParseSep(pc);
			else {
				pc->comment = 0;
				return JimParseStr(pc);
			}
			break;
		case '\n':
		case ';':
			pc->comment = 1;
			if (pc->state == JIM_PS_DEF)
				return JimParseEol(pc);
			else
				return JimParseStr(pc);
			break;
		case '[':
			pc->comment = 0;
			return JimParseCmd(pc);
			break;
		case '$':
			pc->comment = 0;
			if (JimParseVar(pc) == JIM_ERR) {
				pc->tstart = pc->tend = pc->p++;
				pc->tline = pc->linenr;
				pc->tt = JIM_TT_STR;
				return JIM_OK;
			} else
				return JIM_OK;
			break;
		case '#':
			if (pc->comment) {
				JimParseComment(pc);
				continue;
			} else {
				return JimParseStr(pc);
			}
		default:
			pc->comment = 0;
			return JimParseStr(pc);
			break;
		}
		return JIM_OK;
	}
}

int JimParseSep(struct JimParserCtx *pc)
{
	pc->tstart = pc->p;
	pc->tline = pc->linenr;
	while (*pc->p == ' ' || *pc->p == '\t' || *pc->p == '\r' ||
	       (*pc->p == '\\' && *(pc->p+1) == '\n')) {
		if (*pc->p == '\\') pc->p++;
		pc->p++;
	}
	pc->tend = pc->p-1;
	pc->tt = JIM_TT_SEP;
	return JIM_OK;
}

int JimParseEol(struct JimParserCtx *pc)
{
	pc->tstart = pc->p;
	pc->tline = pc->linenr;
	while (*pc->p == ' ' || *pc->p == '\n' ||
	       *pc->p == '\t' || *pc->p == '\r' ||
	       *pc->p == ';') {
		if (*pc->p == '\n')
			pc->linenr++;
		pc->p++;
	}
	pc->tend = pc->p-1;
	pc->tt = JIM_TT_EOL;
	return JIM_OK;
}

/* Todo. Don't stop if ']' appears inside {} or quoted.
 * Also should handle the case of puts [string length "]"] */
int JimParseCmd(struct JimParserCtx *pc)
{
	int level = 1;
	int blevel = 0;

	pc->tstart = ++pc->p;
	pc->tline = pc->linenr;
	while (1) {
		if (*pc->p == '[' && blevel == 0)
			level++;
		else if (*pc->p == ']' && blevel == 0) {
			level--;
			if (!level) break;
		} else if (*pc->p == '\\') {
			pc->p++;
		} else if (*pc->p == '{') {
			blevel++;
		} else if (*pc->p == '}') {
			if (blevel != 0)
				blevel--;
		} else if (*pc->p == '\0') {
			break;
		} else if (*pc->p == '\n')
			pc->linenr++;
		pc->p++;
	}
	pc->tend = pc->p-1;
	pc->tt = JIM_TT_CMD;
	if (*pc->p == ']') pc->p++;
	return JIM_OK;
}

int JimParseVar(struct JimParserCtx *pc)
{
	int brace = 0, stop = 0, ttype = JIM_TT_VAR;

	pc->tstart = ++pc->p; /* skip the $ */
	pc->tline = pc->linenr;
	if (*pc->p == '{') {
		pc->tstart = ++pc->p;
		brace = 1;
	}
	if (brace) {
		while (!stop) {
			if (*pc->p == '}' || *pc->p == '\0') {
				stop = 1;
				if (*pc->p == '\0')
					continue;
			}
			else if (*pc->p == '\n')
				pc->linenr++;
			pc->p++;
		}
		if (*pc->p == '\0')
			pc->tend = pc->p-1;
		else
			pc->tend = pc->p-2;
	} else {
		while (!stop) {
			if (!((*pc->p >= 'a' && *pc->p <= 'z') ||
			    (*pc->p >= 'A' && *pc->p <= 'Z') ||
			    (*pc->p >= '0' && *pc->p <= '9') ||
			    *pc->p == '_'))
				stop = 1;
			else
				pc->p++;
		}
		/* Parse [dict get] syntax sugar. */
		if (*pc->p == '(') {
			while (*pc->p != ')' && *pc->p != '\0') {
				pc->p++;
				if (*pc->p == '\\' && *(pc->p+1) != '\0')
					pc->p+=2;
			}
			if (*pc->p != '\0')
				pc->p++;
			ttype = JIM_TT_DICTSUGAR;
		}
		pc->tend = pc->p-1;
	}
	/* Check if we parsed just the '$' character.
	 * That's not a variable so an error is returned
	 * to tell the state machine to consider this '$' just
	 * a string. */
	if (pc->tstart == pc->p) {
		pc->p--;
		return JIM_ERR;
	}
	pc->tt = ttype;
	return JIM_OK;
}

int JimParseBrace(struct JimParserCtx *pc)
{
	int level = 1;

	pc->tstart = ++pc->p;
	pc->tline = pc->linenr;
	while (1) {
		if (*pc->p == '\\' && *(pc->p+1) != '\0') {
			pc->p++;
		} else if (*pc->p == '{') {
			level++;
		} else if (*pc->p == '\0' || *pc->p == '}') {
			level--;
			if (*pc->p == '\0' || level == 0) {
				pc->tend = pc->p-1;
				if (*pc->p != '\0') pc->p++;
				pc->tt = JIM_TT_STR;
				return JIM_OK;
			}
		} else if (*pc->p == '\n') {
			pc->linenr++;
		}
		pc->p++;
	}
	return JIM_OK; /* unreached */
}

int JimParseStr(struct JimParserCtx *pc)
{
	int newword = (pc->tt == JIM_TT_SEP || pc->tt == JIM_TT_EOL ||
			pc->tt == JIM_TT_NONE || pc->tt == JIM_TT_STR);
	if (newword && *pc->p == '{') {
		return JimParseBrace(pc);
	} else if (newword && *pc->p == '"') {
		pc->state = JIM_PS_QUOTE;
		pc->p++;
	}
	pc->tstart = pc->p;
	pc->tline = pc->linenr;
	while (1) {
		switch(*pc->p) {
		case '\\':
			if (pc->state == JIM_PS_DEF &&
			    *(pc->p+1) == '\n') {
				pc->tend = pc->p-1;
				pc->tt = JIM_TT_ESC;
				return JIM_OK;
			}
			if (*(pc->p+1) != '\0') pc->p++;
			break;
		case '$':
		case '[':
		case '\0':
			pc->tend = pc->p-1;
			pc->tt = JIM_TT_ESC;
			return JIM_OK;
		case ' ':
		case '\t':
		case '\n':
		case '\r':
		case ';':
			if (pc->state == JIM_PS_DEF) {
				pc->tend = pc->p-1;
				pc->tt = JIM_TT_ESC;
				return JIM_OK;
			} else if (*pc->p == '\n') {
				pc->linenr++;
			}
			break;
		case '"':
			if (pc->state == JIM_PS_QUOTE) {
				pc->tend = pc->p-1;
				pc->tt = JIM_TT_ESC;
				pc->p++;
				pc->state = JIM_PS_DEF;
				return JIM_OK;
			}
			break;
		}
		pc->p++;
	}
	return JIM_OK; /* unreached */
}

int JimParseComment(struct JimParserCtx *pc)
{
	while (*pc->p) {
		if (*pc->p == '\n') {
			pc->linenr++;
			if (*(pc->p-1) != '\\') {
				pc->p++;
				return JIM_OK;
			}
		}
		pc->p++;
	}
	return JIM_OK;
}

/* xdigitval and odigitval are helper functions for JimParserGetToken() */
static int xdigitval(int c)
{
	switch(c) {
	case '0': return 0; case '1': return 1; case '2': return 2;
	case '3': return 3; case '4': return 4; case '5': return 5;
	case '6': return 6; case '7': return 7; case '8': return 8;
	case '9': return 9; case 'A': case 'a': return 10;
	case 'B': case 'b': return 11; case 'C': case 'c': return 12;
	case 'D': case 'd': return 13; case 'E': case 'e': return 14;
	case 'F': case 'f': return 15;
	}
	return -1;
}

static int odigitval(int c)
{
	switch(c) {
	case '0': return 0; case '1': return 1; case '2': return 2;
	case '3': return 3; case '4': return 4; case '5': return 5;
	case '6': return 6; case '7': return 7;
	}
	return -1;
}

/* Perform Tcl escape substitution of 's', storing the result
 * string into 'dest'. The escaped string is guaranteed to
 * be the same length or shorted than the source string.
 * Slen is the length of the string at 's', if it's -1 the string
 * length will be calculated by the function.
 *
 * The function returns the length of the resulting string. */
static int Jim_Escape(char *dest, char *s, int slen)
{
	char *p = dest;
	int i, len;
	
	if (slen == -1)
		slen = strlen(s);

	for (i = 0; i < slen; i++) {
		switch(s[i]) {
		case '\\':
			switch(s[i+1]) {
			case 'a': *p++ = 0x7; i++; break;
			case 'b': *p++ = 0x8; i++; break;
			case 'f': *p++ = 0xc; i++; break;
			case 'n': *p++ = 0xa; i++; break;
			case 'r': *p++ = 0xd; i++; break;
			case 't': *p++ = 0x9; i++; break;
			case 'v': *p++ = 0xb; i++; break;
			case '\0': *p++ = '\\'; i++; break;
			default:
				  if (s[i+1] == 'x') {
					int val = 0;
					int c = xdigitval(s[i+2]);
					if (c == -1) {
						*p++ = 'x';
						i++;
						break;
					}
					val = c;
					c = xdigitval(s[i+3]);
					if (c == -1) {
						*p++ = val;
						i += 2;
						break;
					}
					val = (val*16)+c;
					*p++ = val;
					i += 3;
					break;
				  } else if (s[i+1] >= '0' &&
					     s[i+1] <= '7')
				  {
					int val = 0;
					int c = odigitval(s[i+1]);
					val = c;
					c = odigitval(s[i+2]);
					if (c == -1) {
						*p++ = val;
						i ++;
						break;
					}
					val = (val*8)+c;
					c = odigitval(s[i+3]);
					if (c == -1) {
						*p++ = val;
						i += 2;
						break;
					}
					val = (val*8)+c;
					*p++ = val;
					i += 3;
				  } else {
					*p++ = s[i+1];
					i++;
				  }
				  break;
			}
			break;
		default:
			*p++ = s[i];
			break;
		}
	}
	len = p-dest;
	*p++ = '\0';
	return len;
}

/* Returns a dynamically allocated copy of the current token in the
 * parser context. The function perform conversion of escapes if
 * the token is of type JIM_TT_ESC.
 *
 * Note that after the conversion, tokens that are grouped with
 * braces in the source code, are always recognizable from the
 * identical string obtained in a different way from the type.
 *
 * For exmple the string:
 *
 * {expand}$a
 * 
 * will return as first token "expand", of type JIM_TT_STR
 *
 * While the string:
 *
 * expand$a
 *
 * will return as first token "expand", of type JIM_TT_ESC
 */
char *JimParserGetToken(struct JimParserCtx *pc,
		int *lenPtr, int *typePtr, int *linePtr)
{
	char *start, *end, *token;
	int len;

	start = JimParserTstart(pc);
	end = JimParserTend(pc);
	if (start > end) {
		if (lenPtr) *lenPtr = 0;
		if (typePtr) *typePtr = JimParserTtype(pc);
		if (linePtr) *linePtr = JimParserTline(pc);
		return JimEmptyStringRep;
	}
	len = (end-start)+1;
	token = Jim_Alloc(len+1);
	if (JimParserTtype(pc) != JIM_TT_ESC) {
		/* No escape conversion needed? Just copy it. */
		memcpy(token, start, len);
		token[len] = '\0';
	} else {
		/* Else convert the escape chars. */
		len = Jim_Escape(token, start, len);
	}
	if (lenPtr) *lenPtr = len;
	if (typePtr) *typePtr = JimParserTtype(pc);
	if (linePtr) *linePtr = JimParserTline(pc);
	return token;
}

/* -----------------------------------------------------------------------------
 * Tcl Lists parsing
 * ---------------------------------------------------------------------------*/
static int JimParseListSep(struct JimParserCtx *pc);
static int JimParseListStr(struct JimParserCtx *pc);

int JimParseList(struct JimParserCtx *pc)
{
	switch(*pc->p) {
	case '\0':
		pc->tstart = pc->tend = pc->p;
		pc->tline = pc->linenr;
		pc->tt = JIM_TT_EOL;
		pc->eof = 1;
		break;
	case ' ':
	case '\n':
	case '\t':
	case '\r':
		if (pc->state == JIM_PS_DEF)
			return JimParseListSep(pc);
		else
			return JimParseListStr(pc);
		break;
	default:
		return JimParseListStr(pc);
		break;
	}
	return JIM_OK;
}

int JimParseListSep(struct JimParserCtx *pc)
{
	pc->tstart = pc->p;
	pc->tline = pc->linenr;
	while (*pc->p == ' ' || *pc->p == '\t' || *pc->p == '\r' ||
	       *pc->p == '\n')
	{
		pc->p++;
	}
	pc->tend = pc->p-1;
	pc->tt = JIM_TT_SEP;
	return JIM_OK;
}

int JimParseListStr(struct JimParserCtx *pc)
{
	int newword = (pc->tt == JIM_TT_SEP || pc->tt == JIM_TT_EOL ||
			pc->tt == JIM_TT_NONE);
	if (newword && *pc->p == '{') {
		return JimParseBrace(pc);
	} else if (newword && *pc->p == '"') {
		pc->state = JIM_PS_QUOTE;
		pc->p++;
	}
	pc->tstart = pc->p;
	pc->tline = pc->linenr;
	while (1) {
		switch(*pc->p) {
		case '\\':
			pc->p++;
			break;
		case '\0':
			pc->tend = pc->p-1;
			pc->tt = JIM_TT_ESC;
			return JIM_OK;
		case ' ':
		case '\t':
		case '\n':
		case '\r':
			if (pc->state == JIM_PS_DEF) {
				pc->tend = pc->p-1;
				pc->tt = JIM_TT_ESC;
				return JIM_OK;
			} else if (*pc->p == '\n') {
				pc->linenr++;
			}
			break;
		case '"':
			if (pc->state == JIM_PS_QUOTE) {
				pc->tend = pc->p-1;
				pc->tt = JIM_TT_ESC;
				pc->p++;
				pc->state = JIM_PS_DEF;
				return JIM_OK;
			}
			break;
		}
		pc->p++;
	}
	return JIM_OK; /* unreached */
}

/* -----------------------------------------------------------------------------
 * Jim_Obj related functions
 * ---------------------------------------------------------------------------*/

/* Free the internal representation of the object. */
#define Jim_FreeIntRep(i,o) \
	if ((o)->typePtr && (o)->typePtr->freeIntRepProc) \
		(o)->typePtr->freeIntRepProc(i, o)

/* Get the internal representation pointer */
#define Jim_GetIntRepPtr(o) (o)->internalRep.ptr

/* Set the internal representation pointer */
#define Jim_SetIntRepPtr(o, p) \
	(o)->internalRep.ptr = (p)

/* Return a new initialized object. */
Jim_Obj *Jim_NewObj(Jim_Interp *interp)
{
	Jim_Obj *objPtr;

	/* -- Check if there are objects in the free list -- */
	if (interp->freeList != NULL) {
		/* -- Unlink the object from the free list -- */
		objPtr = interp->freeList;
		interp->freeList = objPtr->nextObjPtr;
	} else {
		/* -- No ready to use objects: allocate a new one -- */
		objPtr = Jim_Alloc(sizeof(*objPtr));
	}

	/* Object is returned with refCount of 0. Every
	 * kind of GC implemented should take care to don't try
	 * to scan objects with refCount == 0. */
	objPtr->refCount = 0;
	/* All the other fields are left not initialized to save time.
	 * The caller will probably want set they to the right
	 * value anyway. */

	/* -- Put the object into the live list -- */
	objPtr->prevObjPtr = NULL;
	objPtr->nextObjPtr = interp->liveList;
	if (interp->liveList)
		interp->liveList->prevObjPtr = objPtr;
	interp->liveList = objPtr;

	return objPtr;
}

/* Free an object. Actually objects are never freed, but
 * just moved to the free objects list, where they will be
 * reused by Jim_NewObj(). */
void Jim_FreeObj(Jim_Interp *interp, Jim_Obj *objPtr)
{
	/* Check if the object was already freed, panic. */
	if (objPtr->refCount == -1)  {
		Jim_Panic("Object %p double freed!", objPtr);
	}
	/* Free the string representation */
	if (objPtr->bytes != NULL) {
		if (objPtr->bytes != JimEmptyStringRep)
			Jim_Free(objPtr->bytes);
	}
	/* Free the internal representation */
	Jim_FreeIntRep(interp, objPtr);
	/* Unlink the object from the live objects list */
	if (objPtr->prevObjPtr)
		objPtr->prevObjPtr->nextObjPtr = objPtr->nextObjPtr;
	if (objPtr->nextObjPtr)
		objPtr->nextObjPtr->prevObjPtr = objPtr->prevObjPtr;
	if (interp->liveList == objPtr)
		interp->liveList = objPtr->nextObjPtr;
	/* Link the object into the free objects list */
	objPtr->prevObjPtr = NULL;
	objPtr->nextObjPtr = interp->freeList;
	if (interp->freeList)
		interp->freeList->prevObjPtr = objPtr;
	interp->freeList = objPtr;
	objPtr->refCount = -1;
}

/* Invalidate the string representation of an object. */
void Jim_InvalidateStringRep(Jim_Obj *objPtr)
{
	if (objPtr->bytes != NULL) {
		if (objPtr->bytes != JimEmptyStringRep)
			Jim_Free(objPtr->bytes);
	}
	objPtr->bytes = NULL;
}

#define Jim_SetStringRep(o, b, l) \
	do { (o)->bytes = b; (o)->length = l; } while (0)

/* Set the initial string representation for an object.
 * Does not try to free an old one. */
void Jim_InitStringRep(Jim_Obj *objPtr, char *bytes, int length)
{
	if (length == 0) {
		objPtr->bytes = JimEmptyStringRep;
		objPtr->length = 0;
	} else {
		objPtr->bytes = Jim_Alloc(length+1);
		objPtr->length = length;
		memcpy(objPtr->bytes, bytes, length);
		objPtr->bytes[length] = '\0';
	}
}

/* Duplicate an object. */
Jim_Obj *Jim_DuplicateObj(Jim_Interp *interp, Jim_Obj *objPtr)
{
	Jim_Obj *dupPtr;

	dupPtr = Jim_NewObj(interp);
	if (objPtr->bytes == NULL) {
		/* Object does not have a valid string representation. */
		dupPtr->bytes = NULL;
	} else {
		Jim_InitStringRep(dupPtr, objPtr->bytes, objPtr->length);
	}
	if (objPtr->typePtr != NULL) {
		if (objPtr->typePtr->dupIntRepProc == NULL) {
			dupPtr->internalRep = objPtr->internalRep;
			dupPtr->typePtr = objPtr->typePtr;
		} else {
			objPtr->typePtr->dupIntRepProc(interp, objPtr, dupPtr);
		}
	} else {
		dupPtr->typePtr = NULL;
	}
	return dupPtr;
}

/* Return the string representation for objPtr. If the object
 * string representation is invalid, calls the method to create
 * a new one starting from the internal representation of the object. */
char *Jim_GetString(Jim_Obj *objPtr, int *lenPtr)
{
	if (objPtr->bytes == NULL) {
		/* Invalid string repr. Generate it. */
		if (objPtr->typePtr->updateStringProc == NULL) {
			Jim_Panic("UpdataStringProc called against '%s' type."
					, objPtr->typePtr->name);
		}
		objPtr->typePtr->updateStringProc(objPtr);
	}
	if (lenPtr)
		*lenPtr = objPtr->length;
	return objPtr->bytes;
}

/* -----------------------------------------------------------------------------
 * String Object
 * ---------------------------------------------------------------------------*/
static void DupStringInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr);
static int SetStringFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr);

Jim_ObjType stringObjType = {
	"string",
	NULL,
	DupStringInternalRep,
	NULL,
	JIM_TYPE_REFERENCES,
};

void DupStringInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr)
{
	interp = interp; /* unused */
	/* This is a bit subtle: the only caller of this function
	 * should be Jim_DuplicateObj(), that will copy the
	 * string representaion. After the copy, the duplicated
	 * object will not have more room in teh buffer than
	 * srcPtr->length bytes. So we just set it to length. */
	dupPtr->internalRep.strValue.maxLength = srcPtr->length;
}

int SetStringFromAny(Jim_Interp *interp, Jim_Obj *objPtr)
{
	/* Get a fresh string representation. */
	(void*) Jim_GetString(objPtr, NULL);
	/* Free any other internal representation. */
	Jim_FreeIntRep(interp, objPtr);
	/* Set it as string, i.e. just set the maxLength field. */
	objPtr->typePtr = &stringObjType;
	objPtr->internalRep.strValue.maxLength = objPtr->length;
	return JIM_OK;
}

Jim_Obj *Jim_NewStringObj(Jim_Interp *interp, char *s, int len)
{
	Jim_Obj *objPtr = Jim_NewObj(interp);

	if (len == -1)
		len = strlen(s);
	/* Alloc/Set the string rep. */
	if (len == 0) {
		objPtr->bytes = JimEmptyStringRep;
		objPtr->length = 0;
	} else {
		objPtr->bytes = Jim_Alloc(len+1);
		objPtr->length = len;
		memcpy(objPtr->bytes, s, len);
		objPtr->bytes[len] = '\0';
	}

	/* No typePtr field for the vanilla string object. */
	objPtr->typePtr = NULL;
	return objPtr;
}

/* This version does not try to duplicate the 's' pointer, but
 * use it directly. */
Jim_Obj *Jim_NewStringObjNoAlloc(Jim_Interp *interp, char *s, int len)
{
	Jim_Obj *objPtr = Jim_NewObj(interp);

	if (len == -1)
		len = strlen(s);
	Jim_SetStringRep(objPtr, s, len);
	objPtr->typePtr = NULL;
	return objPtr;
}

/* Low-level string append. Use it only against objects
 * of type "string". */
void StringAppendString(Jim_Obj *objPtr, char *str, int len)
{
	int needlen;

	if (len == -1)
		len = strlen(str);
	needlen = objPtr->length + len;
	if (objPtr->internalRep.strValue.maxLength < needlen) {
		if (objPtr->bytes == JimEmptyStringRep) {
			objPtr->bytes = Jim_Alloc((needlen*2)+1);
		} else {
			objPtr->bytes = Jim_Realloc(objPtr->bytes, (needlen*2)+1);
		}
		objPtr->internalRep.strValue.maxLength = needlen*2;
	}
	memcpy(objPtr->bytes + objPtr->length, str, len);
	objPtr->bytes[objPtr->length+len] = '\0';
	objPtr->length += len;
}

/* Low-level wrapper to append an object. */
void StringAppendObj(Jim_Obj *objPtr, Jim_Obj *appendObjPtr)
{
	int len;
	char *str;

	str = Jim_GetString(appendObjPtr, &len);
	StringAppendString(objPtr, str, len);
}

/* Higher level API to append strings to objects. */
void Jim_AppendString(Jim_Interp *interp, Jim_Obj *objPtr, char *str, int len)
{
	if (Jim_IsShared(objPtr))
		Jim_Panic("Jim_AppendString called with shared object");
	if (objPtr->typePtr != &stringObjType)
		SetStringFromAny(interp, objPtr);
	StringAppendString(objPtr, str, len);
}

void Jim_AppendObj(Jim_Interp *interp, Jim_Obj *objPtr, Jim_Obj *appendObjPtr)
{
	int len;
	char *str;

	str = Jim_GetString(appendObjPtr, &len);
	Jim_AppendString(interp, objPtr, str, len);
}

void Jim_AppendStrings(Jim_Interp *interp, Jim_Obj *objPtr, ...)
{
	va_list ap;

	if (objPtr->typePtr != &stringObjType)
		SetStringFromAny(interp, objPtr);
	va_start(ap, objPtr);
	while (1) {
		char *s = va_arg(ap, char*);

		if (s == NULL) break;
		Jim_AppendString(interp, objPtr, s, -1);
	}
	va_end(ap);
}

int Jim_StringEqObj(Jim_Obj *aObjPtr, Jim_Obj *bObjPtr, int nocase)
{
	char *aStr, *bStr;
	int aLen, bLen, i;

	aStr = Jim_GetString(aObjPtr, &aLen);
	bStr = Jim_GetString(bObjPtr, &bLen);
	if (aLen != bLen) return 0;
	if (nocase == 0)
		return memcmp(aStr, bStr, aLen) == 0;
	for (i = 0; i < aLen; i++) {
		if (tolower((int)aStr[i]) != tolower((int)bStr[i]))
			return 0;
	}
	return 1;
}

int Jim_StringMatchObj(Jim_Obj *patternObjPtr, Jim_Obj *objPtr, int nocase)
{
	char *pattern, *string;

	pattern = Jim_GetString(patternObjPtr, NULL);
	string = Jim_GetString(objPtr, NULL);
	return Jim_StringMatch(pattern, string, nocase);
}

/* Convert a range, as returned by Jim_GetRange(), into
 * an absolute index into an object of the specified length.
 * Indexes that result in an absolute value less than 0 are
 * returned as zero. Indexes resulting in an absolute value
 * greater than the object length are returned as the last
 * element of the object. If outOfRangePtr is not NULL,
 * it's set to 1 on this last two special conditions, otherwise
 * it's set to 0. */
int Jim_RelToAbsIndex(int len, int index, int *outOfRangePtr)
{
	if (outOfRangePtr) *outOfRangePtr = 0;
	if (index >= len) {
		index = len-1;
		if (outOfRangePtr) *outOfRangePtr = 1;
	}
	else if (index < 0) {
		index = len + index;
		if (index < 0) {
			index = 0;
			if (outOfRangePtr) *outOfRangePtr = 1;
		} else if (index >= len) {
			index = len-1;
			if (outOfRangePtr) *outOfRangePtr = 1;
		}
	}
	return index;
}

Jim_Obj *Jim_StringRangeObj(Jim_Interp *interp,
		Jim_Obj *strObjPtr, Jim_Obj *firstObjPtr,
		Jim_Obj *lastObjPtr)
{
	int first, last;
	char *str;
	int len, rangeLen;

	if (Jim_GetIndex(interp, firstObjPtr, &first) != JIM_OK ||
	    Jim_GetIndex(interp, lastObjPtr, &last) != JIM_OK)
		return NULL;
	str = Jim_GetString(strObjPtr, &len);
	first = Jim_RelToAbsIndex(len, first, NULL);
	last = Jim_RelToAbsIndex(len, last, NULL);
	rangeLen = last-first+1;
	if (rangeLen < 0)
		rangeLen = 0;
	return Jim_NewStringObj(interp, str+first, rangeLen);
}

/* -----------------------------------------------------------------------------
 * Compared String Object
 * ---------------------------------------------------------------------------*/

/* This is strange object that allows to compare a C literal string
 * with a Jim object in very short time if the same comparison is done
 * multiple times. For example every time the [if] command is executed,
 * Jim has to check if a given argument is "else". This comparions if
 * the code has no errors are true most of the times, so we can cache
 * inside the object the pointer of the string of the last matching
 * comparison. Because most C compilers perform literal sharing,
 * so that: char *x = "foo", char *y = "foo", will lead to x == y,
 * this works pretty well even if comparisons are at different places
 * inside the C code. */

Jim_ObjType comparedStringObjType = {
	"compared-string",
	NULL,
	NULL,
	NULL,
	JIM_TYPE_REFERENCES,
};

/* The only way this object is exposed to the API is via the following
 * function. Returns true if the string and the object string repr.
 * are the same, otherwise zero is returned. */
int Jim_CompareStringImmediate(Jim_Interp *interp, Jim_Obj *objPtr, char *str)
{
	if (objPtr->typePtr == &comparedStringObjType &&
	    objPtr->internalRep.ptr == str)
		return 1;
	else {
		char *objStr = Jim_GetString(objPtr, NULL);
		if (strcmp(str, objStr) != 0) return 0;
		if (objPtr->typePtr != &comparedStringObjType) {
			Jim_FreeIntRep(interp, objPtr);
			objPtr->typePtr = &comparedStringObjType;
		}
		objPtr->internalRep.ptr = str;
		return 1;
	}
}

/* -----------------------------------------------------------------------------
 * Source Object
 *
 * This object is just a string from the language point of view, but
 * in the internal representation it contains the filename and line number
 * where this given token was read. This information is used by
 * Jim_EvalObj() if the object passed happens to be of type "source".
 *
 * This allows to propagate the information about line numbers and file
 * names and give error messages with absolute line numbers.
 *
 * Note that this object uses shared strings for filenames, and the
 * pointer to the filename together with the line number is taken into
 * the space for the "inline" internal represenation of the Jim_Object,
 * so there is almost memory zero-overhead.
 *
 * Also the object will be converted to something else if the given
 * token it represents in the source file is not something to be
 * evaluated (not a script), and will be specialized in some other way,
 * so the time overhead is alzo null.
 * ---------------------------------------------------------------------------*/

static void FreeSourceInternalRep(Jim_Interp *interp, Jim_Obj *objPtr);
static void DupSourceInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr);

Jim_ObjType sourceObjType = {
	"source",
	FreeSourceInternalRep,
	DupSourceInternalRep,
	NULL,
	JIM_TYPE_REFERENCES,
};

void FreeSourceInternalRep(Jim_Interp *interp, Jim_Obj *objPtr)
{
	Jim_ReleaseSharedString(interp,
			objPtr->internalRep.sourceValue.fileName);
}

void DupSourceInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr)
{
	dupPtr->internalRep.sourceValue.fileName =
		Jim_GetSharedString(interp,
				srcPtr->internalRep.sourceValue.fileName);
	dupPtr->internalRep.sourceValue.lineNumber =
		dupPtr->internalRep.sourceValue.lineNumber;
	dupPtr->typePtr = &sourceObjType;
}

static void Jim_SetSourceInfo(Jim_Interp *interp, Jim_Obj *objPtr,
		char *fileName, int lineNumber)
{
	if (Jim_IsShared(objPtr))
		Jim_Panic("Jim_SetSourceInfo called with shared object");
	if (objPtr->typePtr != NULL)
		Jim_Panic("Jim_SetSourceInfo called with typePtr != NULL");
	objPtr->internalRep.sourceValue.fileName =
		Jim_GetSharedString(interp, fileName);
	objPtr->internalRep.sourceValue.lineNumber = lineNumber;
	objPtr->typePtr = &sourceObjType;
}

/* -----------------------------------------------------------------------------
 * Script Object
 * ---------------------------------------------------------------------------*/

#define JIM_CMDSTRUCT_EXPAND -1

static void FreeScriptInternalRep(Jim_Interp *interp, Jim_Obj *objPtr);
static void DupScriptInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr);
static int SetScriptFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr);

Jim_ObjType scriptObjType = {
	"script",
	FreeScriptInternalRep,
	DupScriptInternalRep,
	NULL,
	JIM_TYPE_REFERENCES,
};

/* The ScriptToken structure represents every token into a scriptObj.
 * Every token contains an associated Jim_Obj that can be specialized
 * by commands operating on it. */
typedef struct ScriptToken {
	int type;
	Jim_Obj *objPtr;
	int linenr;
} ScriptToken;

/* This is the script object internal representation. An array of
 * ScriptToken structures, with an associated command structure array.
 * The command structure is a pre-computed representation of the
 * command length and arguments structure as a simple liner array
 * of integers.
 * 
 * For example the script:
 *
 * puts hello
 * set $i $x$y [foo]BAR
 *
 * will produce a ScriptObj with the following Tokens:
 *
 * ESC puts
 * SEP
 * ESC hello
 * EOL
 * ESC set
 * EOL
 * VAR i
 * SEP
 * VAR x
 * VAR y
 * SEP
 * CMD foo
 * ESC BAR
 * EOL
 *
 * This is a description of the tokens, separators, and of lines.
 * The command structure instead represents the number of arguments
 * of every command, followed by the tokens of which every argument
 * is composed. So for the example script, the cmdstruct array will
 * contain:
 *
 * 2 1 1 4 1 1 2 2
 *
 * Because "puts hello" has two args (2), composed of single tokens (1 1)
 * While "set $i $x$y [foo]BAR" has four (4) args, the first two
 * composed of single tokens (1 1) and the last two of double tokens
 * (2 2).
 *
 * The precomputation of the command structure makes Jim_Eval() faster,
 * and simpler because there aren't dynamic lengths / allocations.
 *
 * -- {expand} handling --
 *
 * Expand is handled in a special way. When a command
 * contains at least an argument with the {expand} prefix,
 * the command structure presents a -1 before the integer
 * describing the number of arguments. This is used in order
 * to send the command exection to a different path in case
 * of {expand} and guarantee a fast path for the more common
 * case. Also, the integers describing the number of tokens
 * are expressed with negative sign, to allow for fast check
 * of what's an {expand}-prefixed argument and what not.
 *
 * For example the command:
 *
 * list {expand}{1 2}
 *
 * Will produce the following cmdstruct array:
 *
 * -1 2 1 -2
 *
 * -- the substFlags field of the structure --
 *
 * The scriptObj structure is used to represent both "script" objects
 * and "subst" objects. In the second case, the cmdStruct related
 * fields are not used at all, but there is an additional field used
 * that is 'substFlags': this represents the flags used to turn
 * the string into the intenral representation used to perform the
 * substitution. If this flags are not what the application requires
 * the scriptObj is created again. For example the script:
 *
 * subst -nocommands $string
 * subst -novariables $string
 *
 * Will recreate the internal representation of the $string object
 * two times.
 */
typedef struct ScriptObj {
	int len; /* Length as number of tokens. */
	int commands; /* number of top-level commands in script. */
	ScriptToken *token; /* Tokens array. */
	int *cmdStruct; /* commands structure */
	int csLen; /* length of the cmdStruct array. */
	int substFlags; /* flags used for the compilation of "subst" objects */
	int inUse; /* Used to share a ScriptObj. Currently
		      only used by Jim_EvalObj() as protection against
		      shimmering of the currently evaluated object. */
	char *fileName;
} ScriptObj;

void FreeScriptInternalRep(Jim_Interp *interp, Jim_Obj *objPtr)
{
	int i;
	struct ScriptObj *script = (void*) objPtr->internalRep.ptr;

	script->inUse--;
	if (script->inUse != 0) return;
	for (i = 0; i < script->len; i++) {
		if (script->token[i].objPtr != NULL)
			Jim_DecrRefCount(interp, script->token[i].objPtr);
	}
	Jim_Free(script->token);
	Jim_Free(script->cmdStruct);
	Jim_Free(script->fileName);
	Jim_Free(script);
}

void DupScriptInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr)
{
	interp = interp;
	srcPtr = srcPtr;

	/* Just returns an simple string. */
	dupPtr->typePtr = NULL;
}

/* Add a new token to the internal repr of a script object */
static void ScriptObjAddToken(Jim_Interp *interp, struct ScriptObj *script,
		char *strtoken, int len, int type, char *filename, int linenr)
{
	int prevtype;
	struct ScriptToken *token;

	prevtype = (script->len == 0) ? JIM_TT_EOL : \
		script->token[script->len-1].type;
	/* Skip tokens without meaning, like words separators
	 * following a word separator or an end of command and
	 * so on. */
	if (prevtype == JIM_TT_EOL) {
		if (type == JIM_TT_EOL || type == JIM_TT_SEP) {
			free(strtoken);
			return;
		}
	} else if (prevtype == JIM_TT_SEP) {
		if (type == JIM_TT_SEP) {
			free(strtoken);
			return;
		} else if (type == JIM_TT_EOL) {
			/* If an EOL is following by a SEP, drop the previous
			 * separator. */
			script->len--;
			Jim_DecrRefCount(interp,
					script->token[script->len].objPtr);
		}
	} else if (prevtype != JIM_TT_EOL && prevtype != JIM_TT_SEP &&
			type == JIM_TT_ESC && len == 0)
	{
		/* Don't add empty tokens used in interpolation */
		return;
	}
	/* Make space for a new istruction */
	script->len++;
	script->token = Jim_Realloc(script->token,
			sizeof(ScriptToken)*script->len);
	/* Initialize the new token */
	token = script->token+(script->len-1);
	token->type = type;
	/* Every object is intially as a string, but the
	 * internal type may be specialized during execution of the
	 * script. */
	token->objPtr = Jim_NewStringObjNoAlloc(interp, strtoken, len);
	/* To add source info to SEP and EOL tokens is useless because
	 * they will never by called as arguments of Jim_EvalObj(). */
	if (filename && type != JIM_TT_SEP && type != JIM_TT_EOL)
		Jim_SetSourceInfo(interp, token->objPtr, filename, linenr);
	Jim_IncrRefCount(token->objPtr);
	token->linenr = linenr;
}

/* Add an integer into the command structure field of the script object. */
static void ScriptObjAddInt(struct ScriptObj *script, int val)
{
	script->csLen++;
	script->cmdStruct = Jim_Realloc(script->cmdStruct,
					sizeof(int)*script->csLen);
	script->cmdStruct[script->csLen-1] = val;
}

/* Search a Jim_Obj contained in 'script' with the same stinrg repr.
 * of objPtr. Search nested script objects recursively. */
static Jim_Obj *ScriptSearchLiteral(Jim_Interp *interp, ScriptObj *script,
		Jim_Obj *objPtr)
{
	int i;

	for (i = 0; i < script->len; i++) {
		if (script->token[i].objPtr != objPtr &&
		    Jim_StringEqObj(script->token[i].objPtr, objPtr, 0))
			return script->token[i].objPtr;
		if (script->token[i].objPtr->typePtr == &scriptObjType) {
			Jim_Obj *foundObjPtr;

			ScriptObj *subScript =
				script->token[i].objPtr->internalRep.ptr;
			foundObjPtr =
				ScriptSearchLiteral(interp, subScript, objPtr);
			if (foundObjPtr != NULL)
				return foundObjPtr;
		}
	}
	return NULL;
}

/* Share literals of a script recursively sharing sub-scripts literals. */
static void ScriptShareLiterals(Jim_Interp *interp, ScriptObj *script,
		ScriptObj *topLevelScript)
{
	int i, j;

	/* Try to share with toplevel object. */
	if (0 && topLevelScript != NULL) {
		for (i = 0; i < script->len; i++) {
			Jim_Obj *foundObjPtr;

			if (script->token[i].objPtr->refCount != 1) continue;
			foundObjPtr = ScriptSearchLiteral(interp,
					topLevelScript,
					script->token[i].objPtr);
			if (foundObjPtr != NULL) {
				Jim_IncrRefCount(foundObjPtr);
				Jim_DecrRefCount(interp,
						script->token[i].objPtr);
				script->token[i].objPtr = foundObjPtr;
			}
		}
	}
	/* Try to share locally */
	for (i = 0; i < script->len; i++) {
		if (script->token[i].objPtr->refCount != 1) continue;
		for (j = 0; j < script->len; j++) {
			if (script->token[i].objPtr !=
					script->token[j].objPtr &&
			    Jim_StringEqObj(script->token[i].objPtr,
				    	    script->token[j].objPtr, 0))
			{
				Jim_IncrRefCount(script->token[j].objPtr);
				Jim_DecrRefCount(interp,
						script->token[i].objPtr);
				script->token[i].objPtr =
					script->token[j].objPtr;
			}
		}
	}
}

/* This method takes the string representation of an object
 * as a Tcl script, and generates the pre-parsed internal representation
 * of the script. */
int SetScriptFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr)
{
	char *scriptText = Jim_GetString(objPtr, NULL);
	struct JimParserCtx parser;
	struct ScriptObj *script = Jim_Alloc(sizeof(*script));
	ScriptToken *token;
	int args, tokens, start, end, i;
	int initialLineNumber;
	int propagateSourceInfo = 0;

	script->len = 0;
	script->csLen = 0;
	script->commands = 0;
	script->token = NULL;
	script->cmdStruct = NULL;
	script->inUse = 1;
	/* Try to get information about filename / line number */
	if (objPtr->typePtr == &sourceObjType) {
		script->fileName =
			Jim_StrDup(objPtr->internalRep.sourceValue.fileName);
		initialLineNumber = objPtr->internalRep.sourceValue.lineNumber;
		propagateSourceInfo = 1;
	} else {
		script->fileName = Jim_StrDup("?");
		initialLineNumber = 1;
	}

	JimParserInit(&parser, scriptText, initialLineNumber);
	while(!JimParserEof(&parser)) {
		char *token;
		int len, type, linenr;

		JimParseScript(&parser);
		token = JimParserGetToken(&parser, &len, &type, &linenr);
		ScriptObjAddToken(interp, script, token, len, type,
				propagateSourceInfo ? script->fileName : NULL,
				linenr);
	}
	token = script->token;

	/* Compute the command structure array
	 * (see the ScriptObj struct definition for more info) */
	start = 0; /* Current command start token index */
	end = -1; /* Current command end token index */
	while (1) {
		int expand = 0; /* expand flag. set to 1 on {expand} form. */
		int interpolation = 0; /* set to 1 if there is at least one
					  argument of the command obtained via
					  interpolation of more tokens. */
		/* Search for the end of command, while
		 * count the number of args. */
		start = ++end;
		if (start >= script->len) break;
		args = 1; /* Number of args in current command */
		while (token[end].type != JIM_TT_EOL) {
			if (end == 0 || token[end-1].type == JIM_TT_SEP ||
					token[end-1].type == JIM_TT_EOL)
			{
				if (token[end].type == JIM_TT_STR &&
				    token[end+1].type != JIM_TT_SEP &&
				    token[end+1].type != JIM_TT_EOL &&
				    !strcmp(token[end].objPtr->bytes, "expand"))
					expand++;
			}
			if (token[end].type == JIM_TT_SEP)
				args++;
			end++;
		}
		interpolation = !((end-start+1) == args*2);
		/* Add the 'number of arguments' info into cmdstruct.
		 * Negative value if there is list expansion involved. */
		if (expand)
			ScriptObjAddInt(script, -1);
		ScriptObjAddInt(script, args);
		/* Now add info about the number of tokens. */
		tokens = 0; /* Number of tokens in current argument. */
		expand = 0;
		for (i = start; i <= end; i++) {
			if (token[i].type == JIM_TT_SEP ||
			    token[i].type == JIM_TT_EOL)
			{
				if (tokens == 1 && expand)
					expand = 0;
				ScriptObjAddInt(script,
						expand ? -tokens : tokens);

				expand = 0;
				tokens = 0;
				continue;
			} else if (tokens == 0 && token[i].type == JIM_TT_STR &&
				   !strcmp(token[i].objPtr->bytes, "expand"))
			{
				expand++;
			}
			tokens++;
		}
	}
	/* Perform literal sharing, but only for objects that appear
	 * to be scripts written as literals inside the source code,
	 * and not computed at runtime. Literal sharing is a costly
	 * operation that should be done only against objects that
	 * are likely to require compilation only the first time, and
	 * then are executed multiple times. */
	if (propagateSourceInfo && interp->framePtr->procBodyObjPtr) {
		Jim_Obj *bodyObjPtr = interp->framePtr->procBodyObjPtr;
		if (bodyObjPtr->typePtr == &scriptObjType) {
			ScriptObj *bodyScript =
				bodyObjPtr->internalRep.ptr;
			ScriptShareLiterals(interp, script, bodyScript);
		}
	} else if (propagateSourceInfo) {
		ScriptShareLiterals(interp, script, NULL);
	}
	/* Free the old internal rep and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	Jim_SetIntRepPtr(objPtr, script);
	objPtr->typePtr = &scriptObjType;
	return JIM_OK;
}

ScriptObj *Jim_GetScript(Jim_Interp *interp, Jim_Obj *objPtr)
{
	if (objPtr->typePtr != &scriptObjType) {
		SetScriptFromAny(interp, objPtr);
	}
	return (ScriptObj*) Jim_GetIntRepPtr(objPtr);
}

/* -----------------------------------------------------------------------------
 * Commands
 * ---------------------------------------------------------------------------*/

/* Commands HashTable Type.
 *
 * Keys are dynamic allocated strings, Values are Jim_Cmd structures. */
static void Jim_CommandsHT_ValDestructor(void *interp, void *val)
{
	Jim_Cmd *cmdPtr = (void*) val;

	if (cmdPtr->cmdProc == NULL) {
		Jim_DecrRefCount(interp, cmdPtr->argListObjPtr);
		Jim_DecrRefCount(interp, cmdPtr->bodyObjPtr);
	}
	Jim_Free(val);
}

Jim_HashTableType Jim_CommandsHashTableType = {
	Jim_StringCopyHT_HashFunction,		/* hash function */
	Jim_StringCopyHT_KeyDup,		/* key dup */
	NULL,					/* val dup */
	Jim_StringCopyHT_KeyCompare,		/* key compare */
	Jim_StringCopyHT_KeyDestructor,		/* key destructor */
	Jim_CommandsHT_ValDestructor		/* val destructor */
};

/* ------------------------- Commands related functions --------------------- */

int Jim_CreateCommand(Jim_Interp *interp, char *cmdName, Jim_CmdProc cmdProc,
		int arityMin, int arityMax, void *privData)
{
	Jim_HashEntry *he;
	Jim_Cmd *cmdPtr;

	he = Jim_FindHashEntry(&interp->commands, cmdName);
	if (he == NULL) { /* New command to create */
		cmdPtr = Jim_Alloc(sizeof(*cmdPtr));
		cmdPtr->cmdProc = cmdProc;
		cmdPtr->arityMin = arityMin;
		cmdPtr->arityMax = arityMax;
		cmdPtr->privData = privData;
		Jim_AddHashEntry(&interp->commands, cmdName, cmdPtr);
	} else {
		/* Free the arglist/body objects if it was a Tcl procedure */
		cmdPtr = he->val;
		if (cmdPtr->cmdProc == NULL) {
			Jim_DecrRefCount(interp, cmdPtr->argListObjPtr);
			Jim_DecrRefCount(interp, cmdPtr->bodyObjPtr);
		}
		cmdPtr->cmdProc = cmdProc;
		cmdPtr->arityMin = arityMin;
		cmdPtr->arityMax = arityMax;
		cmdPtr->privData = privData;
	}
	/* There is no need to increment the 'proc epoch' because
	 * creation of a new procedure can never affect existing
	 * cached commands. We don't do negative caching. */
	return JIM_OK;
}

int Jim_CreateProcedure(Jim_Interp *interp, char *cmdName,
		Jim_Obj *argListObjPtr, Jim_Obj *bodyObjPtr,
		int arityMin, int arityMax)
{
	Jim_HashEntry *he;
	Jim_Cmd *cmdPtr;

	he = Jim_FindHashEntry(&interp->commands, cmdName);
	if (he == NULL) { /* New procedure to create */
		cmdPtr = Jim_Alloc(sizeof(*cmdPtr));
		cmdPtr->cmdProc = NULL; /* Not a C coded command */
		cmdPtr->argListObjPtr = argListObjPtr;
		cmdPtr->bodyObjPtr = bodyObjPtr;
		Jim_IncrRefCount(argListObjPtr);
		Jim_IncrRefCount(bodyObjPtr);
		cmdPtr->arityMin = arityMin;
		cmdPtr->arityMax = arityMax;
		Jim_AddHashEntry(&interp->commands, cmdName, cmdPtr);
	} else {
		/* Free the arglist/body objects if it was a Tcl procedure */
		cmdPtr = he->val;
		if (cmdPtr->cmdProc == NULL) {
			Jim_DecrRefCount(interp, cmdPtr->argListObjPtr);
			Jim_DecrRefCount(interp, cmdPtr->bodyObjPtr);
		}
		cmdPtr->cmdProc = NULL; /* Not a C coded command */
		cmdPtr->arityMin = arityMin;
		cmdPtr->arityMax = arityMax;
		cmdPtr->argListObjPtr = argListObjPtr;
		cmdPtr->bodyObjPtr = bodyObjPtr;
		Jim_IncrRefCount(argListObjPtr);
		Jim_IncrRefCount(bodyObjPtr);
	}
	/* There is no need to increment the 'proc epoch' because
	 * creation of a new procedure can never affect existing
	 * cached commands. We don't do negative caching. */
	return JIM_OK;
}

int Jim_DeleteCommand(Jim_Interp *interp, char *cmdName)
{
	if (Jim_DeleteHashEntry(&interp->commands, cmdName) == JIM_ERR)
		return JIM_ERR;
	Jim_InterpIncrProcEpoch(interp);
	return JIM_OK;
}

int Jim_RenameCommand(Jim_Interp *interp, char *oldName, char *newName)
{
	Jim_Cmd *cmdPtr;
	Jim_HashEntry *he;

	if (newName[0] == '\0') /* Delete! */
		return Jim_DeleteCommand(interp, oldName);
	/* Rename */
	he = Jim_FindHashEntry(&interp->commands, oldName);
	if (he == NULL)
		return JIM_ERR; /* Invalid command name */
	cmdPtr = he->val;
	if (cmdPtr->cmdProc == NULL) {	/* Tcl procedure? */
		Jim_IncrRefCount(cmdPtr->argListObjPtr);
		Jim_IncrRefCount(cmdPtr->bodyObjPtr);
		Jim_CreateProcedure(interp, newName, cmdPtr->argListObjPtr,
				cmdPtr->bodyObjPtr, cmdPtr->arityMin,
				cmdPtr->arityMax);
		Jim_DecrRefCount(interp, cmdPtr->argListObjPtr);
		Jim_DecrRefCount(interp, cmdPtr->bodyObjPtr);
	} else {			/* Or C-coded command. */
		Jim_CreateCommand(interp, newName, cmdPtr->cmdProc,
				cmdPtr->arityMin, cmdPtr->arityMax,
				cmdPtr->privData);
	}
	/* DeleteCommand will incr the proc epoch */
	return Jim_DeleteCommand(interp, oldName);
}

/* -----------------------------------------------------------------------------
 * Command object
 * ---------------------------------------------------------------------------*/

static int SetCommandFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr);

Jim_ObjType commandObjType = {
	"command",
	NULL,
	NULL,
	NULL,
	JIM_TYPE_REFERENCES,
};

int SetCommandFromAny(Jim_Interp *interp, Jim_Obj *objPtr)
{
	Jim_HashEntry *he;
	char *cmdName;

	/* Get the string representation */
	cmdName = Jim_GetString(objPtr, NULL);
	/* Lookup this name into the commands hash table */
	he = Jim_FindHashEntry(&interp->commands, cmdName);
	if (he == NULL)
		return JIM_ERR;

	/* Free the old internal repr and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	objPtr->typePtr = &commandObjType;
	objPtr->internalRep.cmdValue.procEpoch = interp->procEpoch;
	objPtr->internalRep.cmdValue.cmdPtr = (void*)he->val;
	return JIM_OK;
}

/* This function returns the command structure for the command name
 * stored in objPtr. It tries to specialize the objPtr to contain
 * a cached info instead to perform the lookup into the hash table
 * every time. The information cached may not be uptodate, in such
 * a case the lookup is performed and the cache updated. */
Jim_Cmd *Jim_GetCommand(Jim_Interp *interp, Jim_Obj *objPtr, int flags)
{
	if ((objPtr->typePtr != &commandObjType ||
	    objPtr->internalRep.cmdValue.procEpoch != interp->procEpoch) &&
	    SetCommandFromAny(interp, objPtr) == JIM_ERR) {
		if (flags & JIM_ERRMSG) {
			Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
			Jim_AppendStrings(interp, Jim_GetResult(interp),
				"invalid command name \"", objPtr->bytes, "\"",
				NULL);
		}
		return NULL;
	}
	return objPtr->internalRep.cmdValue.cmdPtr;
}

/* -----------------------------------------------------------------------------
 * Variables
 * ---------------------------------------------------------------------------*/

/* Variables HashTable Type.
 *
 * Keys are dynamic allocated strings, Values are Jim_Var structures. */
static void Jim_VariablesHT_ValDestructor(void *interp, void *val)
{
	Jim_Var *varPtr = (void*) val;

	Jim_DecrRefCount(interp, varPtr->objPtr);
	Jim_Free(val);
}

Jim_HashTableType Jim_VariablesHashTableType = {
	Jim_StringCopyHT_HashFunction,		/* hash function */
	Jim_StringCopyHT_KeyDup,		/* key dup */
	NULL,					/* val dup */
	Jim_StringCopyHT_KeyCompare,		/* key compare */
	Jim_StringCopyHT_KeyDestructor,		/* key destructor */
	Jim_VariablesHT_ValDestructor		/* val destructor */
};

/* -----------------------------------------------------------------------------
 * Variable object
 * ---------------------------------------------------------------------------*/

#define JIM_DICT_SUGAR 100 /* Only returned by SetVariableFromAny() */

static int SetVariableFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr);

Jim_ObjType variableObjType = {
	"variable",
	NULL,
	NULL,
	NULL,
	JIM_TYPE_REFERENCES,
};

/* Return true if the string "str" looks like syntax sugar for [dict]. I.e.
 * is in the form "varname(key)". */
static int Jim_NameIsDictSugar(char *str, int len)
{
	if (len == -1)
		len = strlen(str);
	if (len && str[len-1] == ')' && strchr(str, '(') != NULL)
		return 1;
	return 0;
}

/* This method should be called only by the variable API.
 * It returns JIM_OK on success (variable already exists),
 * JIM_ERR if it does not exists, JIM_DICT_GLUE if it's not
 * a variable name, but syntax glue for [dict] i.e. the last
 * character is ')' */
int SetVariableFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr)
{
	Jim_HashEntry *he;
	char *varName;
	int len;

	/* Check if the object is already an uptodate variable */
	if (objPtr->typePtr == &variableObjType &&
	    objPtr->internalRep.varValue.callFrameId == interp->framePtr->id)
		return JIM_OK; /* nothing to do */
	/* Get the string representation */
	varName = Jim_GetString(objPtr, &len);
	//printf("HERE %s\n", varName);
	/* Make sure it's not syntax glue to get/set dict. */
	if (Jim_NameIsDictSugar(varName, len))
			return JIM_DICT_SUGAR;
	/* Lookup this name into the variables hash table */
	he = Jim_FindHashEntry(&interp->framePtr->vars, varName);
	if (he == NULL)
		return JIM_ERR;

	/* Free the old internal repr and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	objPtr->typePtr = &variableObjType;
	objPtr->internalRep.varValue.callFrameId = interp->framePtr->id;
	objPtr->internalRep.varValue.varPtr = (void*)he->val;
	return JIM_OK;
}

/* -------------------- Variables related functions ------------------------- */
static int Jim_DictSugarSet(Jim_Interp *interp, Jim_Obj *ObjPtr,
		Jim_Obj *valObjPtr);
static Jim_Obj *Jim_DictSugarGet(Jim_Interp *interp, Jim_Obj *ObjPtr);

/* For now that's dummy. Variables lookup should be optimized
 * in many ways, with caching of lookups, and possibly with
 * a table of pre-allocated vars in every CallFrame for local vars.
 * All the caching should also have an 'epoch' mechanism similar
 * to the one used by Tcl for procedures lookup caching. */

int Jim_SetVariable(Jim_Interp *interp, Jim_Obj *nameObjPtr, Jim_Obj *valObjPtr)
{
	char *name;
	Jim_Var *var;
	int err;

	if ((err = SetVariableFromAny(interp, nameObjPtr)) != JIM_OK) {
		/* Check for [dict] syntax sugar. */
		if (err == JIM_DICT_SUGAR)
			return Jim_DictSugarSet(interp, nameObjPtr, valObjPtr);
		/* New variable to create */
		name = Jim_GetString(nameObjPtr, NULL);

		var = Jim_Alloc(sizeof(*var));
		var->objPtr = valObjPtr;
		Jim_IncrRefCount(valObjPtr);
		var->linkFramePtr = NULL;
		/* Insert the new variable */
		Jim_AddHashEntry(&interp->framePtr->vars, name, var);
		/* Make the object int rep a variable */
		Jim_FreeIntRep(interp, nameObjPtr);
		nameObjPtr->typePtr = &variableObjType;
		nameObjPtr->internalRep.varValue.callFrameId =
			interp->framePtr->id;
		nameObjPtr->internalRep.varValue.varPtr = var;
	} else {
		var = nameObjPtr->internalRep.varValue.varPtr;
		if (var->linkFramePtr == NULL) {
			Jim_DecrRefCount(interp, var->objPtr);
			var->objPtr = valObjPtr;
			Jim_IncrRefCount(valObjPtr);
		} else { /* Else handle the link */
			Jim_CallFrame *savedCallFrame;

			savedCallFrame = interp->framePtr;
			interp->framePtr = var->linkFramePtr;
			err = Jim_SetVariable(interp, var->objPtr, valObjPtr);
			interp->framePtr = savedCallFrame;
			if (err != JIM_OK)
				return err;
		}
	}
	return JIM_OK;
}

int Jim_SetVariableString(Jim_Interp *interp, char *name, char *val)
{
	Jim_Obj *nameObjPtr, *valObjPtr;
	int result;

	nameObjPtr = Jim_NewStringObj(interp, name, -1);
	valObjPtr = Jim_NewStringObj(interp, val, -1);
	Jim_IncrRefCount(nameObjPtr);
	Jim_IncrRefCount(valObjPtr);
	result = Jim_SetVariable(interp, nameObjPtr, valObjPtr);
	Jim_DecrRefCount(interp, nameObjPtr);
	Jim_DecrRefCount(interp, valObjPtr);
	return result;
}

int Jim_SetVariableLink(Jim_Interp *interp, Jim_Obj *nameObjPtr,
		Jim_Obj *targetNameObjPtr, Jim_CallFrame *targetCallFrame)
{
	char *varName;
	int len;

	/* Check for cycles. */
	if (interp->framePtr == targetCallFrame) {
		Jim_Obj *objPtr = targetNameObjPtr;
		Jim_Var *varPtr;
		/* Cycles are only possible with 'uplevel 0' */
		while(1) {
			if (Jim_StringEqObj(objPtr, nameObjPtr, 0)) {
				Jim_SetResultString(interp,
					"can't upvar from variable to itself",
					-1);
				return JIM_ERR;
			}
			if (SetVariableFromAny(interp, objPtr) != JIM_OK)
				break;
			varPtr = objPtr->internalRep.varValue.varPtr;
			if (varPtr->linkFramePtr != targetCallFrame) break;
			objPtr = varPtr->objPtr;
		}
	}
	varName = Jim_GetString(nameObjPtr, &len);
	if (Jim_NameIsDictSugar(varName, len)) {
		Jim_SetResultString(interp,
			"Dict key syntax invalid as link source", -1);
		return JIM_ERR;
	}
	/* Perform the binding */
	Jim_SetVariable(interp, nameObjPtr, targetNameObjPtr);
	/* We are now sure 'nameObjPtr' type is variableObjType */
	nameObjPtr->internalRep.varValue.varPtr->linkFramePtr = targetCallFrame;
	return JIM_OK;
}

/* Return the Jim_Obj pointer associated with a variable name,
 * or NULL if the variable was not found in the current context.
 * The same optimization discussed in the comment to the
 * 'SetVariable' function should apply here. */
Jim_Obj *Jim_GetVariable(Jim_Interp *interp, Jim_Obj *nameObjPtr, int flags)
{
	int err;

	if ((err = SetVariableFromAny(interp, nameObjPtr)) != JIM_OK) {
		/* Check for [dict] syntax sugar. */
		if (err == JIM_DICT_SUGAR)
			return Jim_DictSugarGet(interp, nameObjPtr);
		if (flags & JIM_ERRMSG) {
			Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
			Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Can't read \"", nameObjPtr->bytes,
				"\": no such variable",
				NULL);
		}
		return NULL;
	} else {
		Jim_Var *varPtr;
		Jim_Obj *objPtr;
		Jim_CallFrame *savedCallFrame;

		varPtr = nameObjPtr->internalRep.varValue.varPtr;
		if (varPtr->linkFramePtr == NULL)
			return varPtr->objPtr;
		/* The variable is a link? Resolve it. */
		savedCallFrame = interp->framePtr;
		interp->framePtr = varPtr->linkFramePtr;
		objPtr = Jim_GetVariable(interp, varPtr->objPtr, JIM_NONE);
		if (objPtr == NULL && flags & JIM_ERRMSG) {
			Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
			Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Can't read \"", nameObjPtr->bytes,
				"\": no such variable",
				NULL);
		}
		interp->framePtr = savedCallFrame;
		return objPtr;
	}
}

Jim_Obj *Jim_GetVariableString(Jim_Interp *interp, char *name, int flags)
{
	Jim_Obj *nameObjPtr, *varObjPtr;

	nameObjPtr = Jim_NewStringObj(interp, name, -1);
	Jim_IncrRefCount(nameObjPtr);
	varObjPtr = Jim_GetVariable(interp, nameObjPtr, flags);
	Jim_DecrRefCount(interp, nameObjPtr);
	return varObjPtr;
}

/* Unset a variable.
 * Note: On success unset invalidates all the variable objects created
 * in the current call frame incrementing. */
int Jim_UnsetVariable(Jim_Interp *interp, Jim_Obj *nameObjPtr, int flags)
{
	char *name;
	Jim_Var *varPtr;
	int err;
	
	if ((err = SetVariableFromAny(interp, nameObjPtr)) != JIM_OK) {
		/* Check for [dict] syntax sugar. */
		if (err == JIM_DICT_SUGAR)
			return Jim_DictSugarSet(interp, nameObjPtr, NULL);
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
			"Can't unset \"", nameObjPtr->bytes,
			"\": no such variable",
			NULL);
		return JIM_ERR; /* var not found */
	}
	varPtr = nameObjPtr->internalRep.varValue.varPtr;
	/* If it's a link call UnsetVariable recursively */
	if (varPtr->linkFramePtr) {
		int retval;

		Jim_CallFrame *savedCallFrame;

		savedCallFrame = interp->framePtr;
		interp->framePtr = varPtr->linkFramePtr;
		retval = Jim_UnsetVariable(interp, varPtr->objPtr, JIM_NONE);
		interp->framePtr = savedCallFrame;
		if (retval != JIM_OK && flags & JIM_ERRMSG) {
			Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
			Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Can't unset \"", nameObjPtr->bytes,
				"\": no such variable",
				NULL);
		}
		return retval;
	} else {
		name = Jim_GetString(nameObjPtr, NULL);
		if (Jim_DeleteHashEntry(&interp->framePtr->vars, name)
				!= JIM_OK) return JIM_ERR;
		/* Change the callframe id, invalidating var lookup caching */
		Jim_ChangeCallFrameId(interp, interp->framePtr);
		return JIM_OK;
	}
}

/* ----------  Dict syntax sugar (similar to array Tcl syntax) -------------- */

/* Given a variable name for [dict] operation syntax sugar,
 * this function returns two objects, the first with the name
 * of the variable to set, and the second with the rispective key.
 * For example "foo(bar)" will return objects with string repr. of
 * "foo" and "bar".
 *
 * The returned objects have refcount = 1. The function can't fail. */
static void Jim_DictSugarParseVarKey(Jim_Interp *interp, Jim_Obj *objPtr,
		Jim_Obj **varPtrPtr, Jim_Obj **keyPtrPtr)
{
	char *str, *p, *t;
	int len, keyLen, nameLen;
	Jim_Obj *varObjPtr, *keyObjPtr;

	str = Jim_GetString(objPtr, &len);
	p = strchr(str, '(');
	p++;
	keyLen = len-((p-str)+1);
	nameLen = (p-str)-1;
	/* Create the objects with the variable name and key. */
	t = Jim_Alloc(nameLen+1);
	memcpy(t, str, nameLen);
	t[nameLen] = '\0';
	varObjPtr = Jim_NewStringObjNoAlloc(interp, t, nameLen);

	t = Jim_Alloc(keyLen+1);
	memcpy(t, p, keyLen);
	t[keyLen] = '\0';
	keyObjPtr = Jim_NewStringObjNoAlloc(interp, t, keyLen);

	Jim_IncrRefCount(varObjPtr);
	Jim_IncrRefCount(keyObjPtr);
	*varPtrPtr = varObjPtr;
	*keyPtrPtr = keyObjPtr;
}

/* Helper of Jim_SetVariable() to deal with dict-syntax variable names.
 * Also used by Jim_UnsetVariable() with valObjPtr = NULL. */
static int Jim_DictSugarSet(Jim_Interp *interp, Jim_Obj *objPtr,
		Jim_Obj *valObjPtr)
{
	Jim_Obj *varObjPtr, *keyObjPtr;
	int err, retval = JIM_OK;

	Jim_DictSugarParseVarKey(interp, objPtr, &varObjPtr, &keyObjPtr);
	err = Jim_SetDictKeysVector(interp, varObjPtr, &keyObjPtr, 1,
			valObjPtr);
	if (err != JIM_OK) {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Variable '", Jim_GetString(varObjPtr, NULL),
				"' does not contain a valid dictionary", NULL);
		retval = err;
	}
	Jim_DecrRefCount(interp, varObjPtr);
	Jim_DecrRefCount(interp, keyObjPtr);
	return retval;
}

/* Helper of Jim_GetVariable() to deal with dict-syntax variable names */
static Jim_Obj *Jim_DictSugarGet(Jim_Interp *interp, Jim_Obj *objPtr)
{
	Jim_Obj *varObjPtr, *keyObjPtr, *dictObjPtr, *resObjPtr;

	Jim_DictSugarParseVarKey(interp, objPtr, &varObjPtr, &keyObjPtr);
	dictObjPtr = Jim_GetVariable(interp, varObjPtr, JIM_ERRMSG);
	if (!dictObjPtr) {
		resObjPtr = NULL;
		goto err;
	}
	if (Jim_DictKey(interp, dictObjPtr, keyObjPtr, &resObjPtr, JIM_ERRMSG)
			!= JIM_OK) {
		resObjPtr = NULL;
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Variable '", Jim_GetString(varObjPtr, NULL),
				"' does not contain a valid dictionary", NULL);
		goto err;
	}
err:
	Jim_DecrRefCount(interp, varObjPtr);
	Jim_DecrRefCount(interp, keyObjPtr);
	return resObjPtr;
}

/* This function is used to expand [dict get] sugar in the form
 * of $var(INDEX). The function is mainly used by Jim_EvalObj()
 * to deal with tokens of type JIM_TT_DICTSUGAR. objPtr points to an
 * object that is *guaranteed* to be in the form VARNAME(INDEX).
 * The 'index' part is [subst]ituted, and is used to lookup a key inside
 * the [dict]ionary contained in variable VARNAME.
 *
 * TODO: It's possible to create a Jim_Obj type with preparsed
 * key/index parts, storing the two parts as objects with the
 * advantage of variable lookup caching and compilation of the
 * index substitution. */
Jim_Obj *Jim_ExpandDictSugar(Jim_Interp *interp, Jim_Obj *objPtr)
{
	Jim_Obj *varObjPtr, *keyObjPtr, *dictObjPtr, *resObjPtr;
	Jim_Obj *substKeyObjPtr = NULL;

	Jim_DictSugarParseVarKey(interp, objPtr, &varObjPtr, &keyObjPtr);
	if (Jim_SubstObj(interp, keyObjPtr, &substKeyObjPtr, JIM_NONE)
			!= JIM_OK) {
		substKeyObjPtr = NULL;
		goto err;
	}
	Jim_IncrRefCount(substKeyObjPtr);
	dictObjPtr = Jim_GetVariable(interp, varObjPtr, JIM_ERRMSG);
	if (!dictObjPtr) {
		resObjPtr = NULL;
		goto err;
	}
	if (Jim_DictKey(interp, dictObjPtr, substKeyObjPtr, &resObjPtr,
				JIM_ERRMSG)
			!= JIM_OK) {
		resObjPtr = NULL;
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Variable '", Jim_GetString(varObjPtr, NULL),
				"' does not contain a valid dictionary", NULL);
		goto err;
	}
err:
	if (substKeyObjPtr) Jim_DecrRefCount(interp, substKeyObjPtr);
	Jim_DecrRefCount(interp, varObjPtr);
	Jim_DecrRefCount(interp, keyObjPtr);
	return resObjPtr;
}

/* -----------------------------------------------------------------------------
 * CallFrame
 * ---------------------------------------------------------------------------*/

static Jim_CallFrame *Jim_CreateCallFrame(Jim_Interp *interp)
{
	Jim_CallFrame *cf;
	if (interp->freeFramesList) {
		cf = interp->freeFramesList;
		interp->freeFramesList = cf->nextFramePtr;
	} else {
		cf = Jim_Alloc(sizeof(*cf));
	}

	cf->id = interp->callFrameEpoch++;
	cf->parentCallFrame = NULL;
	cf->argv = NULL;
	cf->argc = 0;
	cf->procArgsObjPtr = NULL;
	cf->procBodyObjPtr = NULL;
	cf->nextFramePtr = NULL;
	Jim_InitHashTable(&cf->vars, &Jim_VariablesHashTableType, interp);
	return cf;
}

/* Used to invalidate every caching related to callframe stability. */
static void Jim_ChangeCallFrameId(Jim_Interp *interp, Jim_CallFrame *cf)
{
	cf->id = interp->callFrameEpoch++;
}

static void Jim_FreeCallFrame(Jim_Interp *interp, Jim_CallFrame *cf)
{
	if (cf->procArgsObjPtr) Jim_DecrRefCount(interp, cf->procArgsObjPtr);
	if (cf->procBodyObjPtr) Jim_DecrRefCount(interp, cf->procBodyObjPtr);
	Jim_FreeHashTable(&cf->vars);
	cf->nextFramePtr = interp->freeFramesList;
	interp->freeFramesList = cf;
}

/* -----------------------------------------------------------------------------
 * References
 * ---------------------------------------------------------------------------*/

/* References HashTable Type.
 *
 * Keys are jim_wide integers, dynamically allocated for now but in the
 * future it's worth to cache this 8 bytes objects. Values are poitners
 * to Jim_References. */
static void Jim_ReferencesHT_ValDestructor(void *interp, void *val)
{
	Jim_Reference *refPtr = (void*) val;

	Jim_DecrRefCount(interp, refPtr->objPtr);
	if (refPtr->finalizerCmdNamePtr != NULL) {
		Jim_DecrRefCount(interp, refPtr->finalizerCmdNamePtr);
	}
	Jim_Free(val);
}

unsigned int Jim_ReferencesHT_HashFunction(void *key)
{
	/* Only the least significant bits are used. */
	jim_wide *widePtr = key;
	unsigned int intValue = (unsigned int) *widePtr;
	return Jim_IntHashFunction(intValue);
}

unsigned int Jim_ReferencesHT_DoubleHashFunction(void *key)
{
	/* Only the least significant bits are used. */
	jim_wide *widePtr = key;
	unsigned int intValue = (unsigned int) *widePtr;
	return intValue; /* identity function. */
}

void *Jim_ReferencesHT_KeyDup(void *privdata, void *key)
{
	void *copy = Jim_Alloc(sizeof(jim_wide));
	privdata = privdata; /* not used */

	memcpy(copy, key, sizeof(jim_wide));
	return copy;
}

int Jim_ReferencesHT_KeyCompare(void *privdata, void *key1, void *key2)
{
	privdata = privdata; /* not used */

	return memcmp(key1, key2, sizeof(jim_wide)) == 0;
}

void Jim_ReferencesHT_KeyDestructor(void *privdata, void *key)
{
	privdata = privdata; /* not used */

	Jim_Free(key);
}

Jim_HashTableType Jim_ReferencesHashTableType = {
	Jim_ReferencesHT_HashFunction,		/* hash function */
	Jim_ReferencesHT_KeyDup,		/* key dup */
	NULL,					/* val dup */
	Jim_ReferencesHT_KeyCompare,		/* key compare */
	Jim_ReferencesHT_KeyDestructor,		/* key destructor */
	Jim_ReferencesHT_ValDestructor		/* val destructor */
};

/* -----------------------------------------------------------------------------
 * Reference object type and References API
 * ---------------------------------------------------------------------------*/

static void UpdateStringOfReference(struct Jim_Obj *objPtr);

Jim_ObjType referenceObjType = {
	"reference",
	NULL,
	NULL,
	UpdateStringOfReference,
	JIM_TYPE_REFERENCES,
};

void UpdateStringOfReference(struct Jim_Obj *objPtr)
{
	int len;
	char buf[JIM_REFERENCE_SPACE+1];

	len = Jim_WideToReferenceString(buf, objPtr->internalRep.refValue.id);
	objPtr->bytes = Jim_Alloc(len+1);
	memcpy(objPtr->bytes, buf, len+1);
	objPtr->length = len;
}

int SetReferenceFromAny(Jim_Interp *interp, Jim_Obj *objPtr)
{
	jim_wide wideValue;
	int len;
	char *str, *start, *end;
	char refId[21];
	Jim_Reference *refPtr;
	Jim_HashEntry *he;

	/* Get the string representation */
	str = Jim_GetString(objPtr, &len);
	/* Check if it looks like a reference */
	if (len < JIM_REFERENCE_SPACE) goto badformat;
	/* Trim spaces */
	start = str;
	end = str+len-1;
	while (*start == ' ') start++;
	while (*end == ' ' && end > start) end--;
	if (end-start+1 != JIM_REFERENCE_SPACE) goto badformat;
	if (memcmp(start, "~reference:", 11) != 0) goto badformat;
	if (end[0] != ':') goto badformat;
	memcpy(refId, start+11, 20);
	refId[20] = '\0';
	/* Try to convert the ID into a jim_wide */
	if (Jim_StringToWide(refId, &wideValue, 10) != JIM_OK) goto badformat;
	/* Check if the reference really exists! */
	he = Jim_FindHashEntry(&interp->references, &wideValue);
	if (he == NULL) {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Invalid reference ID '", str, "'", NULL);
		return JIM_ERR;
	}
	refPtr = he->val;
	/* Free the old internal repr and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	objPtr->typePtr = &referenceObjType;
	objPtr->internalRep.refValue.id = wideValue;
	objPtr->internalRep.refValue.refPtr = refPtr;
	return JIM_OK;

badformat:
	Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
	Jim_AppendStrings(interp, Jim_GetResult(interp),
			"Expected reference but got '", str, "'", NULL);
	return JIM_ERR;
}

/* Returns a new reference pointing to objPtr, having cmdNamePtr
 * as finalizer command (or NULL if there is no finalizer).
 * The returned reference object has refcount = 0. */
Jim_Obj *Jim_NewReference(Jim_Interp *interp, Jim_Obj *objPtr,
		Jim_Obj *cmdNamePtr)
{
	struct Jim_Reference *refPtr;
	jim_wide wideValue = interp->referenceNextId;
	Jim_Obj *refObjPtr;

	/* Perform the Garbage Collection if needed. */
	Jim_CollectIfNeeded(interp);

	refPtr = Jim_Alloc(sizeof(*refPtr));
	refPtr->objPtr = objPtr;
	Jim_IncrRefCount(objPtr);
	refPtr->finalizerCmdNamePtr = cmdNamePtr;
	if (cmdNamePtr)
		Jim_IncrRefCount(cmdNamePtr);
	Jim_AddHashEntry(&interp->references, &wideValue, refPtr);
	refObjPtr = Jim_NewObj(interp);
	refObjPtr->typePtr = &referenceObjType;
	refObjPtr->bytes = NULL;
	refObjPtr->internalRep.refValue.id = interp->referenceNextId;
	refObjPtr->internalRep.refValue.refPtr = refPtr;
	interp->referenceNextId++;
	return refObjPtr;
}

Jim_Reference *Jim_GetReference(Jim_Interp *interp, Jim_Obj *objPtr)
{
	if (objPtr->typePtr != &referenceObjType &&
		SetReferenceFromAny(interp, objPtr) == JIM_ERR)
		return NULL;
	return objPtr->internalRep.refValue.refPtr;
}

/* -----------------------------------------------------------------------------
 * References Garbage Collection
 * ---------------------------------------------------------------------------*/

/* This the hash table type for the "MARK" phase of the GC */
Jim_HashTableType Jim_RefMarkHashTableType = {
	Jim_ReferencesHT_HashFunction,		/* hash function */
	Jim_ReferencesHT_KeyDup,		/* key dup */
	NULL,					/* val dup */
	Jim_ReferencesHT_KeyCompare,		/* key compare */
	Jim_ReferencesHT_KeyDestructor,		/* key destructor */
	NULL					/* val destructor */
};

/* #define JIM_DEBUG_GC 1 */

/* Performs the garbage collection. */
int Jim_Collect(Jim_Interp *interp)
{
	Jim_HashTable marks;
	Jim_HashTableIterator *htiter;
	Jim_HashEntry *he;
	Jim_Obj *objPtr;
	int collected = 0;

	/* Avoid recursive calls */
	if (interp->lastCollectId == -1) {
		/* Jim_Collect() already running. Return just now. */
		return 0;
	}
	interp->lastCollectId = -1;

	/* Mark all the references found into the 'mark' hash table.
	 * The references are searched in every live object that
	 * is of a type that can contain references. */
	Jim_InitHashTable(&marks, &Jim_RefMarkHashTableType, NULL);
	objPtr = interp->liveList;
	while(objPtr) {
		if (objPtr->typePtr == NULL ||
		    objPtr->typePtr->flags & JIM_TYPE_REFERENCES) {
			char *str, *p;
			int len;

			/* If the object is of type reference, to get the
			 * Id is simple... */
			if (objPtr->typePtr == &referenceObjType) {
				Jim_AddHashEntry(&marks,
					&objPtr->internalRep.refValue.id, NULL);
#ifdef JIM_DEBUG_GC
				printf("MARK (reference): %d refcount: %d\n", 
					(int) objPtr->internalRep.refValue.id,
					objPtr->refCount);
#endif
				objPtr = objPtr->nextObjPtr;
				continue;
			}
			/* Get the string repr of the object we want
			 * to scan for references. */
			p = str = Jim_GetString(objPtr, &len);
			/* Skip objects too little to contain references. */
			if (len < JIM_REFERENCE_SPACE) {
				objPtr = objPtr->nextObjPtr;
				continue;
			}
			/* Extract references from the object string repr. */
			while(1) {
				int i;
				jim_wide id;
				char buf[21];

				if ((p = strstr(p, "~reference:")) == NULL)
					break;
				/* Check if it's a valid reference. */
				if (len-(p-str) < JIM_REFERENCE_SPACE) break;
				if (p[31] != ':') break;
				for (i = 11; i < 30; i++)
					if (!isdigit((int)p[i]))
						break;
				/* Get the ID */
				memcpy(buf, p+11, 20);
				buf[20] = '\0';
				Jim_StringToWide(buf, &id, 10);

				/* Ok, a reference for the given ID
				 * was found. Mark it. */
				Jim_AddHashEntry(&marks, &id, NULL);
#ifdef JIM_DEBUG_GC
				printf("MARK: %d\n", (int)id);
#endif
				p += JIM_REFERENCE_SPACE;
			}
		}
		objPtr = objPtr->nextObjPtr;
	}

	/* Run the references hash table to destroy every reference that
	 * is not referenced outside (not present in the mark HT). */
	htiter = Jim_GetHashTableIterator(&interp->references);
	while ((he = Jim_NextHashEntry(htiter)) != NULL) {
		jim_wide *refId;
		Jim_Reference *refPtr;

		refId = he->key;
		/* Check if in the mark phase we encountered
		 * this reference. */
		if (Jim_FindHashEntry(&marks, refId) == NULL) {
#ifdef JIM_DEBUG_GC
			printf("COLLECTING %d\n", (int)*refId);
#endif
			collected++;
			/* Drop the reference, but call the
			 * finalizer first if registered. */
			refPtr = he->val;
			if (refPtr->finalizerCmdNamePtr) {
				char *refstr = Jim_Alloc(JIM_REFERENCE_SPACE+1);
				Jim_Obj *objv[2], *oldResult;

				Jim_WideToReferenceString(refstr, *refId);

				objv[0] = refPtr->finalizerCmdNamePtr;
				objv[1] = Jim_NewStringObjNoAlloc(interp,
						refstr, 32);
				objv[2] = refPtr->objPtr;
				Jim_IncrRefCount(objv[0]);
				Jim_IncrRefCount(objv[1]);
				Jim_IncrRefCount(objv[2]);

				/* Drop the reference itself */
				Jim_DeleteHashEntry(&interp->references, refId);

				/* Call the finalizer. Errors ignored. */
				oldResult = interp->result;
				Jim_IncrRefCount(oldResult);
				Jim_EvalObjVector(interp, 3, objv);
				Jim_SetResult(interp, oldResult);
				Jim_DecrRefCount(interp, oldResult);

				Jim_DecrRefCount(interp, objv[0]);
				Jim_DecrRefCount(interp, objv[1]);
				Jim_DecrRefCount(interp, objv[2]);
			} else {
				Jim_DeleteHashEntry(&interp->references, refId);
			}
		}
	}
	Jim_FreeHashTableIterator(htiter);
	Jim_FreeHashTable(&marks);
	interp->lastCollectId = interp->referenceNextId;
	interp->lastCollectTime = time(NULL);
	return collected;
}

#define JIM_COLLECT_ID_PERIOD 5000
#define JIM_COLLECT_TIME_PERIOD 300

void Jim_CollectIfNeeded(Jim_Interp *interp)
{
	jim_wide elapsedId;
	int elapsedTime;
	
	elapsedId = interp->referenceNextId - interp->lastCollectId;
	elapsedTime = time(NULL) - interp->lastCollectTime;


	if (elapsedId > JIM_COLLECT_ID_PERIOD ||
	    elapsedTime > JIM_COLLECT_TIME_PERIOD) {
		Jim_Collect(interp);
	}
}

/* -----------------------------------------------------------------------------
 * Interpreter related functions
 * ---------------------------------------------------------------------------*/

Jim_Interp *Jim_CreateInterp(void)
{
	Jim_Interp *i = Jim_Alloc(sizeof(*i));

	i->errorLine = 0;
	i->errorFileName = Jim_StrDup("");
	i->numLevels = 0;
	i->maxNestingDepth = JIM_MAX_NESTING_DEPTH;
	i->returnCode = JIM_OK;
	i->procEpoch = 0;
	i->callFrameEpoch = 0;
	i->liveList = i->freeList = NULL;
	i->scriptFileName = Jim_StrDup("");
	i->referenceNextId = 0;
	i->lastCollectId = 0;
	i->lastCollectTime = time(NULL);
	i->freeFramesList = NULL;

	/* Note that we can create objects only after the
	 * interpreter liveList and freeList pointers are
	 * initialized to NULL. */
	Jim_InitHashTable(&i->commands, &Jim_CommandsHashTableType, i);
	Jim_InitHashTable(&i->references, &Jim_ReferencesHashTableType, i);
	Jim_InitHashTable(&i->sharedStrings, &Jim_SharedStringsHashTableType,
			NULL);
	Jim_InitHashTable(&i->stub, &Jim_StringCopyHashTableType, NULL);
	i->framePtr = i->topFramePtr = Jim_CreateCallFrame(i);
	i->emptyObj = Jim_NewEmptyStringObj(i);
	i->result = i->emptyObj;
	i->stackTrace = Jim_NewListObj(i, NULL, 0);
	i->unknown = Jim_NewStringObj(i, "unknown", -1);
	Jim_IncrRefCount(i->emptyObj);
	Jim_IncrRefCount(i->result);
	Jim_IncrRefCount(i->stackTrace);
	Jim_IncrRefCount(i->unknown);

	/* Initialize key variables every interpreter should contain */
	Jim_SetVariableString(i, "jim::libpath", "./ /usr/local/lib/jim");

	/* Export the core API to extensions */
	Jim_RegisterCoreApi(i);
	return i;
}

void Jim_FreeInterp(Jim_Interp *i)
{
	Jim_CallFrame *cf = i->framePtr, *prevcf, *nextcf;
	Jim_Obj *objPtr, *nextObjPtr;

	Jim_DecrRefCount(i, i->emptyObj);
	Jim_DecrRefCount(i, i->result);
	Jim_DecrRefCount(i, i->stackTrace);
	Jim_DecrRefCount(i, i->unknown);
	Jim_Free(i->errorFileName);
	Jim_Free(i->scriptFileName);
	Jim_FreeHashTable(&i->commands);
	Jim_FreeHashTable(&i->references);
	Jim_FreeHashTable(&i->stub);
	/* Free the call frames list */
	while(cf) {
		prevcf = cf->parentCallFrame;
		Jim_FreeCallFrame(i, cf);
		cf = prevcf;
	}
	/* Check that the live object list is empty, otherwise
	 * there is a memory leak. */
	if (i->liveList != NULL) {
		Jim_Obj *objPtr = i->liveList;
	
		printf("\n-------------------------------------\n");
		printf("Objects still in the free list:\n");
		while(objPtr) {
			char *type = objPtr->typePtr ?
				objPtr->typePtr->name : "";
			printf("%p \"%-10s\": '%.20s' (refCount: %d)\n",
					objPtr, type,
					objPtr->bytes ? objPtr->bytes
					: "(null)", objPtr->refCount);
			if (objPtr->typePtr == &sourceObjType) {
				printf("FILE %s LINE %d\n",
				objPtr->internalRep.sourceValue.fileName,
				objPtr->internalRep.sourceValue.lineNumber);
			}
			objPtr = objPtr->nextObjPtr;
		}
		printf("-------------------------------------\n\n");
		Jim_Panic("Live list non empty freeing the interpreter! Leak?");
	}
	/* Free all the freed objects. */
	objPtr = i->freeList;
	while (objPtr) {
		nextObjPtr = objPtr->nextObjPtr;
		Jim_Free(objPtr);
		objPtr = nextObjPtr;
	}
	/* Free cached CallFrame structures */
	cf = i->freeFramesList;
	while(cf) {
		nextcf = cf->nextFramePtr;
		free(cf);
		cf = nextcf;
	}
	/* Free the sharedString hash table. Make sure to free it
	 * after every other Jim_Object was freed. */
	Jim_FreeHashTable(&i->sharedStrings);
	/* Free the interpreter structure. */
	Jim_Free(i);
}

/* Store the call frame relative to the level represented by
 * levelObjPtr into *framePtrPtr. If levelObjPtr == NULL, the
 * level is assumed to be '1'. */
int Jim_GetCallFrameByLevel(Jim_Interp *interp, Jim_Obj *levelObjPtr,
		Jim_CallFrame **framePtrPtr)
{
	long level;
	char *str;
	Jim_CallFrame *framePtr;

	if (levelObjPtr) {
		str = Jim_GetString(levelObjPtr, NULL);
		if (str[0] == '#') {
			char *endptr;
			/* speedup for the toplevel (level #0) */
			if (str[1] == '0' && str[2] == '\0') {
				*framePtrPtr = interp->topFramePtr;
				return JIM_OK;
			}

			str++;
			level = strtol(str, &endptr, 0);
			if (str[0] == '\0' || endptr[0] != '\0' || level < 0)
				goto badlevel;
			/* An 'absolute' level is converted into the
			 * 'number of levels to go back' format. */
			level = interp->numLevels - level;
			if (level < 0) goto badlevel;
		} else {
			if (Jim_GetLong(interp, levelObjPtr, &level) != JIM_OK
				|| level < 0)
				goto badlevel;
		}
	} else {
		level = 1;
	}
	/* Lookup */
	framePtr = interp->framePtr;
	while (level--) {
		framePtr = framePtr->parentCallFrame;
		if (framePtr == NULL) goto badlevel;
	}
	*framePtrPtr = framePtr;
	return JIM_OK;
badlevel:
	Jim_SetResultString(interp, "Bad level", -1);
	return JIM_ERR;
}

static void Jim_SetErrorFileName(Jim_Interp *interp, char *filename)
{
	Jim_Free(interp->errorFileName);
	interp->errorFileName = Jim_StrDup(filename);
}

static void Jim_SetErrorLineNumber(Jim_Interp *interp, int linenr)
{
	interp->errorLine = linenr;
}

static void Jim_ResetStackTrace(Jim_Interp *interp)
{
	Jim_DecrRefCount(interp, interp->stackTrace);
	interp->stackTrace = Jim_NewListObj(interp, NULL, 0);
	Jim_IncrRefCount(interp->stackTrace);
}

static void Jim_AppendStackTrace(Jim_Interp *interp, char *procname,
		char *filename, int linenr)
{
	if (Jim_IsShared(interp->stackTrace)) {
		interp->stackTrace =
			Jim_DuplicateObj(interp, interp->stackTrace);
		Jim_IncrRefCount(interp->stackTrace);
	}
	Jim_ListAppendElement(interp, interp->stackTrace,
			Jim_NewStringObj(interp, procname, -1));
	Jim_ListAppendElement(interp, interp->stackTrace,
			Jim_NewStringObj(interp, filename, -1));
	Jim_ListAppendElement(interp, interp->stackTrace,
			Jim_NewIntObj(interp, linenr));
}

/* -----------------------------------------------------------------------------
 * Shared strings.
 * Every interpreter has an hash table where to put shared dynamically
 * allocate strings that are likely to be used a lot of times.
 * For example, in the 'source' object type, there is a pointer to
 * the filename associated with that object. Every script has a lot
 * of this objects with the identical file name, so it is wise to share
 * this info.
 *
 * The API is trivial: Jim_GetSharedString(interp, "foobar")
 * returns the pointer to the shared string. Every time a reference
 * to the string is no longer used, the user should call
 * Jim_ReleaseSharedString(interp, stringPointer). Once no one is using
 * a given string, it is removed from the hash table.
 * ---------------------------------------------------------------------------*/
char *Jim_GetSharedString(Jim_Interp *interp, char *str)
{
	Jim_HashEntry *he = Jim_FindHashEntry(&interp->sharedStrings, str);

	if (he == NULL) {
		char *strCopy = Jim_StrDup(str);

		Jim_AddHashEntry(&interp->sharedStrings, strCopy, (void*)1);
		return strCopy;
	} else {
		long refCount = (long) he->val;

		refCount++;
		he->val = (void*) refCount;
		return he->key;
	}
}

void Jim_ReleaseSharedString(Jim_Interp *interp, char *str)
{
	long refCount;
	Jim_HashEntry *he = Jim_FindHashEntry(&interp->sharedStrings, str);

	if (he == NULL)
		Jim_Panic("Jim_ReleaseSharedString called with "
			  "unknown shared string '%s'", str);
	refCount = (long) he->val;
	refCount--;
	if (refCount == 0) {
		Jim_DeleteHashEntry(&interp->sharedStrings, str);
	} else {
		he->val = (void*) refCount;
	}
}

/* -----------------------------------------------------------------------------
 * Integer object
 * ---------------------------------------------------------------------------*/
#define JIM_INTEGER_SPACE 24

static void UpdateStringOfInt(struct Jim_Obj *objPtr);
static int SetIntFromAny(Jim_Interp *interp, Jim_Obj *objPtr);

Jim_ObjType intObjType = {
	"int",
	NULL,
	NULL,
	UpdateStringOfInt,
	JIM_TYPE_NONE,
};

void UpdateStringOfInt(struct Jim_Obj *objPtr)
{
	int len;
	char buf[JIM_INTEGER_SPACE+1];

	len = Jim_WideToString(buf, objPtr->internalRep.wideValue);
	objPtr->bytes = Jim_Alloc(len+1);
	memcpy(objPtr->bytes, buf, len+1);
	objPtr->length = len;
}

int SetIntFromAny(Jim_Interp *interp, Jim_Obj *objPtr)
{
	jim_wide wideValue;
	char *str;

	/* Get the string representation */
	str = Jim_GetString(objPtr, NULL);
	/* Try to convert into a jim_wide */
	if (Jim_StringToWide(str, &wideValue, 0) != JIM_OK) {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Expected integer but got '", str, "'", NULL);
		return JIM_ERR;
	}
	if ((wideValue == JIM_WIDE_MIN || wideValue == JIM_WIDE_MAX) &&
	    errno == ERANGE) {
		Jim_SetResultString(interp,
			"Integer value too big to be represented", -1);
		return JIM_ERR;
	}
	/* Free the old internal repr and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	objPtr->typePtr = &intObjType;
	objPtr->internalRep.wideValue = wideValue;
	return JIM_OK;
}

int Jim_GetWide(Jim_Interp *interp, Jim_Obj *objPtr, jim_wide *widePtr)
{
	if (objPtr->typePtr != &intObjType &&
		SetIntFromAny(interp, objPtr) == JIM_ERR)
		return JIM_ERR;
	*widePtr = objPtr->internalRep.wideValue;
	return JIM_OK;
}

int Jim_GetLong(Jim_Interp *interp, Jim_Obj *objPtr, long *longPtr)
{
	jim_wide wideValue;
	int retval;

	retval = Jim_GetWide(interp, objPtr, &wideValue);
	if (retval == JIM_OK) {
		*longPtr = (long) wideValue;
		return JIM_OK;
	}
	return JIM_ERR;
}

void Jim_SetWide(Jim_Interp *interp, Jim_Obj *objPtr, jim_wide wideValue)
{
	if (Jim_IsShared(objPtr))
		Jim_Panic("Jim_SetWide called with shared object");
	if (objPtr->typePtr != &intObjType) {
		Jim_FreeIntRep(interp, objPtr);
		objPtr->typePtr = &intObjType;
	}
	Jim_InvalidateStringRep(objPtr);
	objPtr->internalRep.wideValue = wideValue;
}

Jim_Obj *Jim_NewIntObj(Jim_Interp *interp, jim_wide wideValue)
{
	Jim_Obj *objPtr;

	objPtr = Jim_NewObj(interp);
	objPtr->typePtr = &intObjType;
	objPtr->bytes = NULL;
	objPtr->internalRep.wideValue = wideValue;
	return objPtr;
}

/* -----------------------------------------------------------------------------
 * Double object
 * ---------------------------------------------------------------------------*/
#define JIM_DOUBLE_SPACE 30

static void UpdateStringOfDouble(struct Jim_Obj *objPtr);
static int SetDoubleFromAny(Jim_Interp *interp, Jim_Obj *objPtr);

Jim_ObjType doubleObjType = {
	"double",
	NULL,
	NULL,
	UpdateStringOfDouble,
	JIM_TYPE_NONE,
};

void UpdateStringOfDouble(struct Jim_Obj *objPtr)
{
	int len;
	char buf[JIM_DOUBLE_SPACE+1];

	len = Jim_DoubleToString(buf, objPtr->internalRep.doubleValue);
	objPtr->bytes = Jim_Alloc(len+1);
	memcpy(objPtr->bytes, buf, len+1);
	objPtr->length = len;
}

int SetDoubleFromAny(Jim_Interp *interp, Jim_Obj *objPtr)
{
	double doubleValue;
	char *str;

	/* Get the string representation */
	str = Jim_GetString(objPtr, NULL);
	/* Try to convert into a double */
	if (Jim_StringToDouble(str, &doubleValue) != JIM_OK) {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Expected number but got '", str, "'", NULL);
		return JIM_ERR;
	}
	/* Free the old internal repr and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	objPtr->typePtr = &doubleObjType;
	objPtr->internalRep.doubleValue = doubleValue;
	return JIM_OK;
}

int Jim_GetDouble(Jim_Interp *interp, Jim_Obj *objPtr, double *doublePtr)
{
	if (objPtr->typePtr != &doubleObjType &&
		SetDoubleFromAny(interp, objPtr) == JIM_ERR)
		return JIM_ERR;
	*doublePtr = objPtr->internalRep.doubleValue;
	return JIM_OK;
}

void Jim_SetDouble(Jim_Interp *interp, Jim_Obj *objPtr, double doubleValue)
{
	if (Jim_IsShared(objPtr))
		Jim_Panic("Jim_SetDouble called with shared object");
	if (objPtr->typePtr != &doubleObjType) {
		Jim_FreeIntRep(interp, objPtr);
		objPtr->typePtr = &doubleObjType;
	}
	Jim_InvalidateStringRep(objPtr);
	objPtr->internalRep.doubleValue = doubleValue;
}

Jim_Obj *Jim_NewDoubleObj(Jim_Interp *interp, double doubleValue)
{
	Jim_Obj *objPtr;

	objPtr = Jim_NewObj(interp);
	objPtr->typePtr = &doubleObjType;
	objPtr->bytes = NULL;
	objPtr->internalRep.doubleValue = doubleValue;
	return objPtr;
}

/* -----------------------------------------------------------------------------
 * List object
 * ---------------------------------------------------------------------------*/
static void ListAppendElement(Jim_Obj *listPtr, Jim_Obj *objPtr);
static void FreeListInternalRep(Jim_Interp *interp, Jim_Obj *objPtr);
static void DupListInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr);
static void UpdateStringOfList(struct Jim_Obj *objPtr);
static int SetListFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr);

/* Note that while the elements of the list may contain references,
 * the list object itself can't. This basically means that the
 * list object string representation as a whole can't contain references
 * that are not presents in the single elements. */
Jim_ObjType listObjType = {
	"list",
	FreeListInternalRep,
	DupListInternalRep,
	UpdateStringOfList,
	JIM_TYPE_NONE,
};

void FreeListInternalRep(Jim_Interp *interp, Jim_Obj *objPtr)
{
	int i;

	for (i = 0; i < objPtr->internalRep.listValue.len; i++) {
		Jim_DecrRefCount(interp, objPtr->internalRep.listValue.ele[i]);
	}
	Jim_Free(objPtr->internalRep.listValue.ele);
}

void DupListInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr)
{
	int i;
	interp = interp; /* unused */

	dupPtr->internalRep.listValue.len = srcPtr->internalRep.listValue.len;
	dupPtr->internalRep.listValue.maxLen =
		srcPtr->internalRep.listValue.maxLen;
	dupPtr->internalRep.listValue.ele =
		Jim_Alloc(sizeof(Jim_Obj*)*srcPtr->internalRep.listValue.maxLen);
	memcpy(dupPtr->internalRep.listValue.ele,
			srcPtr->internalRep.listValue.ele,
			sizeof(Jim_Obj*)*srcPtr->internalRep.listValue.len);
	for (i = 0; i < dupPtr->internalRep.listValue.len; i++) {
		Jim_IncrRefCount(dupPtr->internalRep.listValue.ele[i]);
	}
	dupPtr->typePtr = &listObjType;
}

/* The following function checks if a given string can be encoded
 * into a list element without any kind of quoting, surrounded by braces,
 * or using escapes to quote. */
#define JIM_ELESTR_SIMPLE 0
#define JIM_ELESTR_BRACE 1
#define JIM_ELESTR_QUOTE 2
static int ListElementQuotingType(char *s, int len)
{
	int i, level, trySimple = 1;

	/* Try with the SIMPLE case */
	if (len == 0) return JIM_ELESTR_BRACE;
	if (s[0] == '"' || s[0] == '{') {
		trySimple = 0;
		goto testbrace;
	}
	for (i = 0; i < len; i++) {
		switch(s[i]) {
		case ' ':
		case '$':
		case '"':
		case '[':
		case ']':
		case ';':
		case '\\':
		case '\r':
		case '\n':
		case '\t':
		case '\f':
		case '\v':
			trySimple = 0;
		case '{':
		case '}':
			goto testbrace;
		}
	}
	return JIM_ELESTR_SIMPLE;

testbrace:
	/* Test if it's possible to do with braces */
	if (s[len-1] == '\\' ||
	    s[len-1] == ']') return JIM_ELESTR_QUOTE;
	level = 0;
	for (i = 0; i < len; i++) {
		switch(s[i]) {
		case '{': level++; break;
		case '}': level--;
			  if (level < 0) return JIM_ELESTR_QUOTE;
			  break;
		case '\\':
			  if (s[i+1] == '\n')
				  return JIM_ELESTR_QUOTE;
			  else
				  if (s[i+1] != '\0') i++;
			  break;
		}
	}
	if (level == 0) {
		if (!trySimple) return JIM_ELESTR_BRACE;
		for (i = 0; i < len; i++) {
			switch(s[i]) {
			case ' ':
			case '$':
			case '"':
			case '[':
			case ']':
			case ';':
			case '\\':
			case '\r':
			case '\n':
			case '\t':
			case '\f':
			case '\v':
				return JIM_ELESTR_BRACE;
				break;
			}
		}
		return JIM_ELESTR_SIMPLE;
	}
	return JIM_ELESTR_QUOTE;
}

/* Returns the malloc-ed representation of a string
 * using backslash to quote special chars. */
char *BackslashQuoteString(char *s, int len, int *qlenPtr)
{
	char *q = Jim_Alloc(len*2+1), *p;

	p = q;
	while(*s) {
		switch (*s) {
		case ' ':
		case '$':
		case '"':
		case '[':
		case ']':
		case '{':
		case '}':
		case ';':
		case '\\':
			*p++ = '\\';
			*p++ = *s++;
			break;
		case '\n': *p++ = '\\'; *p++ = 'n'; s++; break;
		case '\r': *p++ = '\\'; *p++ = 'r'; s++; break;
		case '\t': *p++ = '\\'; *p++ = 't'; s++; break;
		case '\f': *p++ = '\\'; *p++ = 'f'; s++; break;
		case '\v': *p++ = '\\'; *p++ = 'v'; s++; break;
		default:
			*p++ = *s++;
			break;
		}
	}
	*p = '\0';
	*qlenPtr = p-q;
	return q;
}

void UpdateStringOfList(struct Jim_Obj *objPtr)
{
	int i, bufLen, realLength;
	char *strRep, *p;
	int *quotingType;
	Jim_Obj **ele = objPtr->internalRep.listValue.ele;

	/* (Over) Estimate the space needed. */
	quotingType = Jim_Alloc(sizeof(int)*objPtr->internalRep.listValue.len);
	bufLen = 0;
	for (i = 0; i < objPtr->internalRep.listValue.len; i++) {
		int len;

		strRep = Jim_GetString(ele[i], &len);
		quotingType[i] = ListElementQuotingType(strRep, len);
		switch (quotingType[i]) {
		case JIM_ELESTR_SIMPLE:
			bufLen += len;
			break;
		case JIM_ELESTR_BRACE:
			bufLen += len+2;
			break;
		case JIM_ELESTR_QUOTE:
			bufLen += len*2;
			break;
		}
		bufLen++; /* elements separator. */
	}
	bufLen++;

	/* Generate the string rep. */
	p = objPtr->bytes = Jim_Alloc(bufLen+1);
	realLength = 0;
	for (i = 0; i < objPtr->internalRep.listValue.len; i++) {
		int len, qlen;
		char *strRep = Jim_GetString(ele[i], &len), *q;

		switch(quotingType[i]) {
		case JIM_ELESTR_SIMPLE:
			memcpy(p, strRep, len);
			p += len;
			realLength += len;
			break;
		case JIM_ELESTR_BRACE:
			*p++ = '{';
			memcpy(p, strRep, len);
			p += len;
			*p++ = '}';
			realLength += len+2;
			break;
		case JIM_ELESTR_QUOTE:
			q = BackslashQuoteString(strRep, len, &qlen);
			memcpy(p, q, qlen);
			Jim_Free(q);
			p += qlen;
			realLength += qlen;
			break;
		}
		/* Add a separating space */
		if (i+1 != objPtr->internalRep.listValue.len) {
			*p++ = ' ';
			realLength ++;
		}
	}
	*p = '\0'; /* nul term. */
	objPtr->length = realLength;
	free(quotingType);
}

int SetListFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr)
{
	struct JimParserCtx parser;
	char *str;

	/* Get the string representation */
	str = Jim_GetString(objPtr, NULL);

	/* Free the old internal repr just now and initialize the
	 * new one just now. The string->list conversion can't fail. */
	Jim_FreeIntRep(interp, objPtr);
	objPtr->typePtr = &listObjType;
	objPtr->internalRep.listValue.len = 0;
	objPtr->internalRep.listValue.maxLen = 0;
	objPtr->internalRep.listValue.ele = NULL;

	/* Convert into a list */
	JimParserInit(&parser, str, 1);
	while(!JimParserEof(&parser)) {
		char *token;
		int tokenLen, type;
		Jim_Obj *elementPtr;

		JimParseList(&parser);
		if (JimParserTtype(&parser) != JIM_TT_STR &&
		    JimParserTtype(&parser) != JIM_TT_ESC)
			continue;
		token = JimParserGetToken(&parser, &tokenLen, &type, NULL);
		elementPtr = Jim_NewStringObjNoAlloc(interp, token, tokenLen);
		ListAppendElement(objPtr, elementPtr);
	}
	return JIM_OK;
}

Jim_Obj *Jim_NewListObj(Jim_Interp *interp, Jim_Obj **elements, int len)
{
	Jim_Obj *objPtr;
	int i;

	objPtr = Jim_NewObj(interp);
	objPtr->typePtr = &listObjType;
	objPtr->bytes = NULL;
	objPtr->internalRep.listValue.ele = NULL;
	objPtr->internalRep.listValue.len = 0;
	objPtr->internalRep.listValue.maxLen = 0;
	for (i = 0; i < len; i++) {
		ListAppendElement(objPtr, elements[i]);
	}
	return objPtr;
}

/* This is the low-level function to append an element to a list.
 * The higher-level Jim_ListAppendElement() performs shared object
 * check and invalidate the string repr. This version is used
 * in the internals of the List Object and is not exported.
 *
 * NOTE: this function can be called only against objects
 * with internal type of List. */
void ListAppendElement(Jim_Obj *listPtr, Jim_Obj *objPtr)
{
	int requiredLen = listPtr->internalRep.listValue.len + 1;

	if (requiredLen > listPtr->internalRep.listValue.maxLen) {
		int maxLen = requiredLen * 2;

		listPtr->internalRep.listValue.ele =
			Jim_Realloc(listPtr->internalRep.listValue.ele,
					sizeof(Jim_Obj*)*maxLen);
		listPtr->internalRep.listValue.maxLen = maxLen;
	}
	listPtr->internalRep.listValue.ele[listPtr->internalRep.listValue.len] =
		objPtr;
	listPtr->internalRep.listValue.len ++;
	Jim_IncrRefCount(objPtr);
}

/* Appends every element of appendListPtr into listPtr.
 * Both have to be of the list type. */
void ListAppendList(Jim_Obj *listPtr, Jim_Obj *appendListPtr)
{
	int i, oldLen = listPtr->internalRep.listValue.len;
	int appendLen = appendListPtr->internalRep.listValue.len;
	int requiredLen = oldLen + appendLen;

	if (requiredLen > listPtr->internalRep.listValue.maxLen) {
		int maxLen = requiredLen * 2;

		listPtr->internalRep.listValue.ele =
			Jim_Realloc(listPtr->internalRep.listValue.ele,
					sizeof(Jim_Obj*)*maxLen);
		listPtr->internalRep.listValue.maxLen = maxLen;
	}
	for (i = 0; i < appendLen; i++) {
		Jim_Obj *objPtr = appendListPtr->internalRep.listValue.ele[i];
		listPtr->internalRep.listValue.ele[oldLen+i] = objPtr;
		Jim_IncrRefCount(objPtr);
	}
	listPtr->internalRep.listValue.len += appendLen;
}

void Jim_ListAppendElement(Jim_Interp *interp, Jim_Obj *listPtr, Jim_Obj *objPtr)
{
	if (Jim_IsShared(listPtr))
		Jim_Panic("Jim_ListAppendElement called with shared object");
	if (listPtr->typePtr != &listObjType)
		SetListFromAny(interp, listPtr);
	Jim_InvalidateStringRep(listPtr);
	ListAppendElement(listPtr, objPtr);
}

void Jim_ListAppendList(Jim_Interp *interp, Jim_Obj *listPtr, Jim_Obj *appendListPtr)
{
	if (Jim_IsShared(listPtr))
		Jim_Panic("Jim_ListAppendList called with shared object");
	if (listPtr->typePtr != &listObjType)
		SetListFromAny(interp, listPtr);
	Jim_InvalidateStringRep(listPtr);
	ListAppendList(listPtr, appendListPtr);
}

void Jim_ListLength(Jim_Interp *interp, Jim_Obj *listPtr, int *intPtr)
{
	if (listPtr->typePtr != &listObjType)
		SetListFromAny(interp, listPtr);
	*intPtr = listPtr->internalRep.listValue.len;
}

int Jim_ListIndex(Jim_Interp *interp, Jim_Obj *listPtr, int index,
		Jim_Obj **objPtrPtr, int flags)
{
	if (listPtr->typePtr != &listObjType)
		SetListFromAny(interp, listPtr);
	if ((index >= 0 && index >= listPtr->internalRep.listValue.len) ||
	    (index < 0 && (-index-1) >= listPtr->internalRep.listValue.len)) {
		if (flags & JIM_ERRMSG) {
			Jim_SetResultString(interp,
				"list index out of range", -1);
		}
		return JIM_ERR;
	}
	if (index < 0)
		index = listPtr->internalRep.listValue.len+index;
	*objPtrPtr = listPtr->internalRep.listValue.ele[index];
	return JIM_OK;
}

static int ListSetIndex(Jim_Interp *interp, Jim_Obj *listPtr, int index,
		Jim_Obj *newObjPtr, int flags)
{
	if (listPtr->typePtr != &listObjType)
		SetListFromAny(interp, listPtr);
	if ((index >= 0 && index >= listPtr->internalRep.listValue.len) ||
	    (index < 0 && (-index-1) >= listPtr->internalRep.listValue.len)) {
		if (flags & JIM_ERRMSG) {
			Jim_SetResultString(interp,
				"list index out of range", -1);
		}
		return JIM_ERR;
	}
	if (index < 0)
		index = listPtr->internalRep.listValue.len+index;
	Jim_DecrRefCount(interp, listPtr->internalRep.listValue.ele[index]);
	listPtr->internalRep.listValue.ele[index] = newObjPtr;
	Jim_IncrRefCount(newObjPtr);
	return JIM_OK;
}

/* Modify the list stored into the variable named 'varNamePtr'
 * setting the element specified by the 'indexc' indexes objects in 'indexv',
 * with the new element 'newObjptr'. */
int Jim_SetListIndex(Jim_Interp *interp, Jim_Obj *varNamePtr, Jim_Obj **indexv,
		int indexc, Jim_Obj *newObjPtr)
{
	Jim_Obj *varObjPtr, *objPtr, *listObjPtr;
	int shared, i, index;

	varObjPtr = objPtr = Jim_GetVariable(interp, varNamePtr, JIM_ERRMSG);
	if (objPtr == NULL)
		return JIM_ERR;
	if ((shared = Jim_IsShared(objPtr)))
		varObjPtr = objPtr = Jim_DuplicateObj(interp, objPtr);
	for (i = 0; i < indexc-1; i++) {
		listObjPtr = objPtr;
		if (Jim_GetIndex(interp, indexv[i], &index) != JIM_OK)
			goto err;
		if (Jim_ListIndex(interp, listObjPtr, index, &objPtr,
					JIM_ERRMSG) != JIM_OK) {
			goto err;
		}
		if (Jim_IsShared(objPtr)) {
			objPtr = Jim_DuplicateObj(interp, objPtr);
			ListSetIndex(interp, listObjPtr, index, objPtr,
					JIM_NONE);
		}
		Jim_InvalidateStringRep(listObjPtr);
	}
	if (Jim_GetIndex(interp, indexv[indexc-1], &index) != JIM_OK)
		goto err;
	if (ListSetIndex(interp, objPtr, index, newObjPtr, JIM_ERRMSG)
			== JIM_ERR) goto err;
	Jim_InvalidateStringRep(objPtr);
	Jim_InvalidateStringRep(varObjPtr);
	if (shared) {
		if (Jim_SetVariable(interp, varNamePtr, varObjPtr) != JIM_OK)
			goto err;
	}
	Jim_SetResult(interp, varObjPtr);
	return JIM_OK;
err:
	if (shared) {
		Jim_IncrRefCount(varObjPtr);
		Jim_DecrRefCount(interp, varObjPtr);
	}
	return JIM_ERR;
}

Jim_Obj *Jim_ConcatObj(Jim_Interp *interp, int objc, Jim_Obj **objv)
{
	int i;

	/* If all the objects in objv are lists without string rep.
	 * it's possible to return a list as result, that's the
	 * concatenation of all the lists. */
	for (i = 0; i < objc; i++) {
		if (objv[i]->typePtr != &listObjType || objv[i]->bytes)
			break;
	}
	if (i == objc) {
		Jim_Obj *objPtr = Jim_NewListObj(interp, NULL, 0);
		for (i = 0; i < objc; i++)
			Jim_ListAppendList(interp, objPtr, objv[i]);
		return objPtr;
	} else {
		/* Else... we have to glue strings together */
		int len = 0, objLen;
		char *bytes, *p;

		/* Compute the length */
		for (i = 0; i < objc; i++) {
			Jim_GetString(objv[i], &objLen);
			len += objLen;
		}
		if (objc) len += objc-1;
		/* Create the string rep, and a stinrg object holding it. */
		p = bytes = Jim_Alloc(len+1);
		for (i = 0; i < objc; i++) {
			char *s = Jim_GetString(objv[i], &objLen);
			while (objLen && (*s == ' ' || *s == '\t' ||
						*s == '\n'))
			{
				s++; objLen--; len--;
			}
			while (objLen && (s[objLen-1] == ' ' ||
				s[objLen-1] == '\n' || s[objLen-1] == '\t')) {
				objLen--; len--;
			}
			memcpy(p, s, objLen);
			p += objLen;
			if (objLen && i+1 != objc) {
				*p++ = ' ';
			} else if (i+1 != objc) {
				/* Drop the space calcuated for this
				 * element that is instead null. */
				len--;
			}
		}
		*p = '\0';
		return Jim_NewStringObjNoAlloc(interp, bytes, len);
	}
}

/* -----------------------------------------------------------------------------
 * Dict object
 * ---------------------------------------------------------------------------*/
static void FreeDictInternalRep(Jim_Interp *interp, Jim_Obj *objPtr);
static void DupDictInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr);
static void UpdateStringOfDict(struct Jim_Obj *objPtr);
static int SetDictFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr);

/* Dict HashTable Type.
 *
 * Keys and Values are Jim objects. */

unsigned int Jim_ObjectHT_HashFunction(void *key)
{
	char *str;
	int len;

	str = Jim_GetString(key, &len);
	return Jim_DjbHashFunction(str, len);
}

int Jim_ObjectHT_KeyCompare(void *privdata, void *key1, void *key2)
{
	privdata = privdata; /* not used */

	return Jim_StringEqObj(key1, key2, 0);
}

static void Jim_ObjectHT_KeyValDestructor(void *interp, void *val)
{
	Jim_Obj *objPtr = val;

	Jim_DecrRefCount(interp, objPtr);
}

Jim_HashTableType Jim_DictHashTableType = {
	Jim_ObjectHT_HashFunction,		/* hash function */
	NULL,					/* key dup */
	NULL,					/* val dup */
	Jim_ObjectHT_KeyCompare,		/* key compare */
	Jim_ObjectHT_KeyValDestructor,		/* key destructor */
	Jim_ObjectHT_KeyValDestructor		/* val destructor */
};

/* Note that while the elements of the dict may contain references,
 * the list object itself can't. This basically means that the
 * dict object string representation as a whole can't contain references
 * that are not presents in the single elements. */
Jim_ObjType dictObjType = {
	"dict",
	FreeDictInternalRep,
	DupDictInternalRep,
	UpdateStringOfDict,
	JIM_TYPE_NONE,
};

void FreeDictInternalRep(Jim_Interp *interp, Jim_Obj *objPtr)
{
	interp = interp; /* not used */

	Jim_FreeHashTable(objPtr->internalRep.ptr);
	Jim_Free(objPtr->internalRep.ptr);
}

void DupDictInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr)
{
	Jim_HashTable *ht, *dupHt;
	Jim_HashTableIterator *htiter;
	Jim_HashEntry *he;

	/* Create a new hash table */
	ht = srcPtr->internalRep.ptr;
	dupHt = Jim_Alloc(sizeof(*dupHt));
	Jim_InitHashTable(dupHt, &Jim_DictHashTableType, interp);
	if (ht->size != 0)
		Jim_ExpandHashTable(dupHt, ht->size);
	/* Copy every element from the source to the dup hash table */
	htiter = Jim_GetHashTableIterator(ht);
	while ((he = Jim_NextHashEntry(htiter)) != NULL) {
		Jim_Obj *keyObjPtr = he->key;
		Jim_Obj *valObjPtr = he->val;

		Jim_IncrRefCount(keyObjPtr);
		Jim_IncrRefCount(valObjPtr);
		Jim_AddHashEntry(dupHt, keyObjPtr, valObjPtr);
	}
	Jim_FreeHashTableIterator(htiter);

	dupPtr->internalRep.ptr = dupHt;
	dupPtr->typePtr = &dictObjType;
}

void UpdateStringOfDict(struct Jim_Obj *objPtr)
{
	int i, bufLen, realLength;
	char *strRep, *p;
	int *quotingType, objc;
	Jim_HashTable *ht;
	Jim_HashTableIterator *htiter;
	Jim_HashEntry *he;
	Jim_Obj **objv;

	/* Trun the hash table into a flat vector of Jim_Objects. */
	ht = objPtr->internalRep.ptr;
	objc = ht->used*2;
	objv = Jim_Alloc(objc*sizeof(Jim_Obj*));
	htiter = Jim_GetHashTableIterator(ht);
	i = 0;
	while ((he = Jim_NextHashEntry(htiter)) != NULL) {
		objv[i++] = he->key;
		objv[i++] = he->val;
	}
	Jim_FreeHashTableIterator(htiter);
	/* (Over) Estimate the space needed. */
	quotingType = Jim_Alloc(sizeof(int)*objc);
	bufLen = 0;
	for (i = 0; i < objc; i++) {
		int len;

		strRep = Jim_GetString(objv[i], &len);
		quotingType[i] = ListElementQuotingType(strRep, len);
		switch (quotingType[i]) {
		case JIM_ELESTR_SIMPLE:
			bufLen += len;
			break;
		case JIM_ELESTR_BRACE:
			bufLen += len+2;
			break;
		case JIM_ELESTR_QUOTE:
			bufLen += len*2;
			break;
		}
		bufLen++; /* elements separator. */
	}
	bufLen++;

	/* Generate the string rep. */
	p = objPtr->bytes = Jim_Alloc(bufLen+1);
	realLength = 0;
	for (i = 0; i < objc; i++) {
		int len, qlen;
		char *strRep = Jim_GetString(objv[i], &len), *q;

		switch(quotingType[i]) {
		case JIM_ELESTR_SIMPLE:
			memcpy(p, strRep, len);
			p += len;
			realLength += len;
			break;
		case JIM_ELESTR_BRACE:
			*p++ = '{';
			memcpy(p, strRep, len);
			p += len;
			*p++ = '}';
			realLength += len+2;
			break;
		case JIM_ELESTR_QUOTE:
			q = BackslashQuoteString(strRep, len, &qlen);
			memcpy(p, q, qlen);
			Jim_Free(q);
			p += qlen;
			realLength += qlen;
			break;
		}
		/* Add a separating space */
		if (i+1 != objc) {
			*p++ = ' ';
			realLength ++;
		}
	}
	*p = '\0'; /* nul term. */
	objPtr->length = realLength;
	free(quotingType);
	free(objv);
}

int SetDictFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr)
{
	struct JimParserCtx parser;
	Jim_HashTable *ht;
	Jim_Obj *objv[2];
	char *str;
	int i;

	/* Get the string representation */
	str = Jim_GetString(objPtr, NULL);

	/* Free the old internal repr just now and initialize the
	 * new one just now. The string->list conversion can't fail. */
	Jim_FreeIntRep(interp, objPtr);
	ht = Jim_Alloc(sizeof(*ht));
	Jim_InitHashTable(ht, &Jim_DictHashTableType, interp);
	objPtr->typePtr = &dictObjType;
	objPtr->internalRep.ptr = ht;

	/* Convert into a dict */
	JimParserInit(&parser, str, 1);
	i = 0;
	while(!JimParserEof(&parser)) {
		char *token;
		int tokenLen, type;

		JimParseList(&parser);
		if (JimParserTtype(&parser) != JIM_TT_STR &&
		    JimParserTtype(&parser) != JIM_TT_ESC)
			continue;
		token = JimParserGetToken(&parser, &tokenLen, &type, NULL);
		objv[i++] = Jim_NewStringObjNoAlloc(interp, token, tokenLen);
		if (i == 2) {
			i = 0;
			Jim_IncrRefCount(objv[0]);
			Jim_IncrRefCount(objv[1]);
			if (Jim_AddHashEntry(ht, objv[0], objv[1]) != JIM_OK) {
				Jim_HashEntry *he;

				he = Jim_FindHashEntry(ht, objv[0]);
				Jim_DecrRefCount(interp, objv[0]);
				Jim_DecrRefCount(interp, (Jim_Obj*)he->val);
				he->val = objv[1];
			}
		}
	}
	if (i) {
		Jim_IncrRefCount(objv[0]);
		Jim_DecrRefCount(interp, objv[0]);
		objPtr->typePtr = NULL;
		Jim_FreeHashTable(ht);
		Jim_SetResultString(interp, "missing value to go with key", -1);
		return JIM_ERR;
	}
	return JIM_OK;
}

/* Dict object API */

/* Add an element to a dict. objPtr must be of the "dict" type.
 * The higer-level exported function is Jim_DictAddElement().
 * If an element with the specified key already exists, the value
 * associated is replaced with the new one.
 *
 * if valueObjPtr == NULL, the key is instead removed if it exists. */
static void DictAddElement(Jim_Interp *interp, Jim_Obj *objPtr,
		Jim_Obj *keyObjPtr, Jim_Obj *valueObjPtr)
{
	Jim_HashTable *ht = objPtr->internalRep.ptr;

	if (valueObjPtr == NULL) { /* unset */
		Jim_DeleteHashEntry(ht, keyObjPtr);
		return;
	}
	Jim_IncrRefCount(keyObjPtr);
	Jim_IncrRefCount(valueObjPtr);
	if (Jim_AddHashEntry(ht, keyObjPtr, valueObjPtr) != JIM_OK) {
		Jim_HashEntry *he = Jim_FindHashEntry(ht, keyObjPtr);
		Jim_DecrRefCount(interp, keyObjPtr);
		Jim_DecrRefCount(interp, (Jim_Obj*)he->val);
		he->val = valueObjPtr;
	}
}

/* Add an element, higher-level interface for DictAddElement().
 * If valueObjPtr == NULL, the key is removed if it exists. */
int Jim_DictAddElement(Jim_Interp *interp, Jim_Obj *objPtr,
		Jim_Obj *keyObjPtr, Jim_Obj *valueObjPtr)
{
	if (Jim_IsShared(objPtr))
		Jim_Panic("Jim_DictAddElement called with shared object");
	if (objPtr->typePtr != &dictObjType) {
		if (SetDictFromAny(interp, objPtr) != JIM_OK)
			return JIM_ERR;
	}
	DictAddElement(interp, objPtr, keyObjPtr, valueObjPtr);
	Jim_InvalidateStringRep(objPtr);
	return JIM_OK;
}

Jim_Obj *Jim_NewDictObj(Jim_Interp *interp, Jim_Obj **elements, int len)
{
	Jim_Obj *objPtr;
	int i;

	if (len % 2)
		Jim_Panic("Jim_NewDicObj() 'len' argument must be even");

	objPtr = Jim_NewObj(interp);
	objPtr->typePtr = &dictObjType;
	objPtr->bytes = NULL;
	objPtr->internalRep.ptr = Jim_Alloc(sizeof(Jim_HashTable));
	Jim_InitHashTable(objPtr->internalRep.ptr, &Jim_DictHashTableType,
			interp);
	for (i = 0; i < len; i += 2)
		DictAddElement(interp, objPtr, elements[i], elements[i+1]);
	return objPtr;
}

/* Return the value associated to the specified dict key */
int Jim_DictKey(Jim_Interp *interp, Jim_Obj *dictPtr, Jim_Obj *keyPtr,
		Jim_Obj **objPtrPtr, int flags)
{
	Jim_HashEntry *he;
	Jim_HashTable *ht;

	if (dictPtr->typePtr != &dictObjType) {
		if (SetDictFromAny(interp, dictPtr) != JIM_OK)
			return JIM_ERR;
	}
	ht = dictPtr->internalRep.ptr;
	if ((he = Jim_FindHashEntry(ht, keyPtr)) == NULL) {
		if (flags & JIM_ERRMSG) {
			Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
			Jim_AppendStrings(interp, Jim_GetResult(interp),
					"key \"", Jim_GetString(keyPtr, NULL),
					"\" not found in dictionary", NULL);
		}
		return JIM_ERR;
	}
	*objPtrPtr = he->val;
	return JIM_OK;
}

/* Return the value associated to the specified dict keys */
int Jim_DictKeysVector(Jim_Interp *interp, Jim_Obj *dictPtr,
		Jim_Obj **keyv, int keyc, Jim_Obj **objPtrPtr, int flags)
{
	Jim_Obj *objPtr;
	int i;

	if (keyc == 0) {
		*objPtrPtr = dictPtr;
		return JIM_OK;
	}

	for (i = 0; i < keyc; i++) {
		if (Jim_DictKey(interp, dictPtr, keyv[i], &objPtr, flags)
				!= JIM_OK)
			return JIM_ERR;
		dictPtr = objPtr;
	}
	*objPtrPtr = objPtr;
	return JIM_OK;
}

/* Modify the dict stored into the variable named 'varNamePtr'
 * setting the element specified by the 'keyc' keys objects in 'keyv',
 * with the new value of the element 'newObjPtr'.
 *
 * If newObjPtr == NULL the operation is to remove the given key
 * from the dictionary. */
int Jim_SetDictKeysVector(Jim_Interp *interp, Jim_Obj *varNamePtr,
		Jim_Obj **keyv, int keyc, Jim_Obj *newObjPtr)
{
	Jim_Obj *varObjPtr, *objPtr, *dictObjPtr;
	int shared, i;

	varObjPtr = objPtr = Jim_GetVariable(interp, varNamePtr, JIM_ERRMSG);
	if (objPtr == NULL) {
		varObjPtr = objPtr = Jim_NewDictObj(interp, NULL, 0);
		if (Jim_SetVariable(interp, varNamePtr, objPtr) != JIM_OK) {
			Jim_IncrRefCount(varObjPtr);
			Jim_DecrRefCount(interp, varObjPtr);
			return JIM_ERR;
		}
	}
	if ((shared = Jim_IsShared(objPtr)))
		varObjPtr = objPtr = Jim_DuplicateObj(interp, objPtr);
	for (i = 0; i < keyc-1; i++) {
		dictObjPtr = objPtr;

		/* Check if it's a valid dictionary */
		if (dictObjPtr->typePtr != &dictObjType) {
			if (SetDictFromAny(interp, dictObjPtr) != JIM_OK)
				goto err;
		}
		/* Check if the given key exists. */
		Jim_InvalidateStringRep(dictObjPtr);
		if (Jim_DictKey(interp, dictObjPtr, keyv[i], &objPtr,
			newObjPtr ? JIM_NONE : JIM_ERRMSG) == JIM_OK)
		{
			/* This key exists at the current level.
			 * Make sure it's not shared!. */
			if (Jim_IsShared(objPtr)) {
				objPtr = Jim_DuplicateObj(interp, objPtr);
				DictAddElement(interp, dictObjPtr,
						keyv[i], objPtr);
			}
		} else {
			/* Key not found. If it's an [unset] operation
			 * this is an error. Only the last key may not
			 * exist. */
			if (newObjPtr == NULL)
				goto err;
			/* Otherwise set an empty dictionary
			 * as key's value. */
			objPtr = Jim_NewDictObj(interp, NULL, 0);
			DictAddElement(interp, dictObjPtr, keyv[i], objPtr);
		}
	}
	if (Jim_DictAddElement(interp, objPtr, keyv[keyc-1], newObjPtr)
			!= JIM_OK)
		goto err;
	Jim_InvalidateStringRep(objPtr);
	Jim_InvalidateStringRep(varObjPtr);
	if (shared) {
		if (Jim_SetVariable(interp, varNamePtr, varObjPtr) != JIM_OK)
			goto err;
	}
	Jim_SetResult(interp, varObjPtr);
	return JIM_OK;
err:
	if (shared) {
		Jim_IncrRefCount(varObjPtr);
		Jim_DecrRefCount(interp, varObjPtr);
	}
	return JIM_ERR;
}


/* -----------------------------------------------------------------------------
 * Index object
 * ---------------------------------------------------------------------------*/
static void UpdateStringOfIndex(struct Jim_Obj *objPtr);
static int SetIndexFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr);

Jim_ObjType indexObjType = {
	"index",
	NULL,
	NULL,
	UpdateStringOfIndex,
	JIM_TYPE_NONE,
};

void UpdateStringOfIndex(struct Jim_Obj *objPtr)
{
	int len;
	char buf[JIM_INTEGER_SPACE+1];

	if (objPtr->internalRep.indexValue >= 0)
		len = sprintf(buf, "%d", objPtr->internalRep.indexValue);
	else if (objPtr->internalRep.indexValue == -1)
		len = sprintf(buf, "end");
	else {
		len = sprintf(buf, "end%d", objPtr->internalRep.indexValue+1);
	}
	objPtr->bytes = Jim_Alloc(len+1);
	memcpy(objPtr->bytes, buf, len+1);
	objPtr->length = len;
}

int SetIndexFromAny(Jim_Interp *interp, Jim_Obj *objPtr)
{
	int index, end = 0;
	char *str, *endptr;

	/* Get the string representation */
	str = Jim_GetString(objPtr, NULL);
	/* Try to convert into an index */
	if (!strcmp(str, "end")) {
		index = 0;
		end = 1;
	} else {
		if (!strncmp(str, "end-", 4)) {
			str += 4;
			end = 1;
		}
		index = strtol(str, &endptr, 0);
		if (str[0] == '\0' || endptr[0] != '\0') {
			Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
			Jim_AppendStrings(interp, Jim_GetResult(interp),
					"bad index \"", str, "\": "
					"must be integer or end?-integer?",
					NULL);
			return JIM_ERR;
		}
	}
	if (end) {
		if (index < 0)
			index = INT_MAX;
		else
			index = -(index+1);
	} else if (!end && index < 0)
		index = INT_MAX;
	/* Free the old internal repr and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	objPtr->typePtr = &indexObjType;
	objPtr->internalRep.indexValue = index;
	return JIM_OK;
}

int Jim_GetIndex(Jim_Interp *interp, Jim_Obj *objPtr, int *indexPtr)
{
	/* Avoid shimmering if the object is an integer. */
	if (objPtr->typePtr == &intObjType) {
		jim_wide val = objPtr->internalRep.wideValue;
		if (!(val < LONG_MIN) && !(val > LONG_MAX)) {
			*indexPtr = (val < 0) ? INT_MAX : (long)val;;
			return JIM_OK;
		}
	}
	if (objPtr->typePtr != &indexObjType &&
		SetIndexFromAny(interp, objPtr) == JIM_ERR)
		return JIM_ERR;
	*indexPtr = objPtr->internalRep.indexValue;
	return JIM_OK;
}

/* -----------------------------------------------------------------------------
 * Return Code Object.
 * ---------------------------------------------------------------------------*/

static int SetReturnCodeFromAny(Jim_Interp *interp, Jim_Obj *objPtr);

Jim_ObjType returnCodeObjType = {
	"return-code",
	NULL,
	NULL,
	NULL,
	JIM_TYPE_NONE,
};

int SetReturnCodeFromAny(Jim_Interp *interp, Jim_Obj *objPtr)
{
	char *str;
	int returnCode;

	/* Get the string representation */
	str = Jim_GetString(objPtr, NULL);
	/* Try to convert into a jim_wide */
	if (!strcasecmp(str, "ok"))
		returnCode = JIM_OK;
	else if (!strcasecmp(str, "error"))
		returnCode = JIM_ERR;
	else if (!strcasecmp(str, "return"))
		returnCode = JIM_RETURN;
	else if (!strcasecmp(str, "break"))
		returnCode = JIM_BREAK;
	else if (!strcasecmp(str, "continue"))
		returnCode = JIM_CONTINUE;
	else {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
				"Expected return code but got '", str, "'",
				NULL);
		return JIM_ERR;
	}
	/* Free the old internal repr and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	objPtr->typePtr = &returnCodeObjType;
	objPtr->internalRep.returnCode = returnCode;
	return JIM_OK;
}

int Jim_GetReturnCode(Jim_Interp *interp, Jim_Obj *objPtr, int *intPtr)
{
	if (objPtr->typePtr != &returnCodeObjType &&
		SetReturnCodeFromAny(interp, objPtr) == JIM_ERR)
		return JIM_ERR;
	*intPtr = objPtr->internalRep.returnCode;
	return JIM_OK;
}

/* -----------------------------------------------------------------------------
 * Expression Parsing
 * ---------------------------------------------------------------------------*/
static int JimParseExprOperator(struct JimParserCtx *pc);
static int JimParseExprNumber(struct JimParserCtx *pc);

/* Operators table */
typedef struct Jim_ExprOperator {
	char *name;
	int precedence;
	int arity;
	int opcode;
} Jim_ExprOperator;

/* Exrp's Stack machine operators opcodes. */

/* Operators */
#define JIM_EXPROP_NOT 0
#define JIM_EXPROP_BITNOT 1
#define JIM_EXPROP_UNARYMINUS 2
#define JIM_EXPROP_UNARYPLUS 3

#define JIM_EXPROP_MUL 4
#define JIM_EXPROP_DIV 5
#define JIM_EXPROP_MOD 6

#define JIM_EXPROP_SUB 7
#define JIM_EXPROP_ADD 8

#define JIM_EXPROP_LSHIFT 9
#define JIM_EXPROP_RSHIFT 10
#define JIM_EXPROP_ROTL 30
#define JIM_EXPROP_ROTR 31

#define JIM_EXPROP_LT 11
#define JIM_EXPROP_GT 12
#define JIM_EXPROP_LTE 13
#define JIM_EXPROP_GTE 14

#define JIM_EXPROP_NUMEQ 15
#define JIM_EXPROP_NUMNE 16

#define JIM_EXPROP_STREQ 17
#define JIM_EXPROP_STRNE 18

#define JIM_EXPROP_BITAND 19
#define JIM_EXPROP_BITXOR 20
#define JIM_EXPROP_BITOR 21

#define JIM_EXPROP_LOGICAND 22
#define JIM_EXPROP_LOGICOR 23

#define JIM_EXPROP_TERNARY 24

/* Operands */
#define JIM_EXPROP_NUMBER 25
#define JIM_EXPROP_COMMAND 26
#define JIM_EXPROP_VARIABLE 27
#define JIM_EXPROP_DICTSUGAR 28
#define JIM_EXPROP_STRING 29

static struct Jim_ExprOperator Jim_ExprOperators[] = {
	{"!", 300, 1, JIM_EXPROP_NOT},
	{"~", 300, 1, JIM_EXPROP_BITNOT},
	{"unarymin", 300, 1, JIM_EXPROP_UNARYMINUS},
	{"unaryplus", 300, 1, JIM_EXPROP_UNARYPLUS},

	{"*", 200, 2, JIM_EXPROP_MUL},
	{"/", 200, 2, JIM_EXPROP_DIV},
	{"%", 200, 2, JIM_EXPROP_MOD},

	{"-", 100, 2, JIM_EXPROP_SUB},
	{"+", 100, 2, JIM_EXPROP_ADD},

	{"<<<", 90, 3, JIM_EXPROP_ROTL},
	{">>>", 90, 3, JIM_EXPROP_ROTR},
	{"<<", 90, 2, JIM_EXPROP_LSHIFT},
	{">>", 90, 2, JIM_EXPROP_RSHIFT},

	{"<",  80, 2, JIM_EXPROP_LT},
	{">",  80, 2, JIM_EXPROP_GT},
	{"<=", 80, 2, JIM_EXPROP_LTE},
	{">=", 80, 2, JIM_EXPROP_GTE},

	{"==", 70, 2, JIM_EXPROP_NUMEQ},
	{"!=", 70, 2, JIM_EXPROP_NUMNE},

	{"eq", 60, 2, JIM_EXPROP_STREQ},
	{"ne", 60, 2, JIM_EXPROP_STRNE},

	{"&", 50, 2, JIM_EXPROP_BITAND},
	{"^", 49, 2, JIM_EXPROP_BITXOR},
	{"|", 48, 2, JIM_EXPROP_BITOR},

	{"&&", 10, 2, JIM_EXPROP_LOGICAND},
	{"||", 10, 2, JIM_EXPROP_LOGICOR},

	{"?", 5, 3, JIM_EXPROP_TERNARY},
};

#define JIM_EXPR_OPERATORS_NUM \
	(sizeof(Jim_ExprOperators)/sizeof(struct Jim_ExprOperator))

int JimParseExpression(struct JimParserCtx *pc)
{
	/* Discard spaces and quoted newline */
	while(*(pc->p) == ' ' ||
	      *(pc->p) == '\t' ||
	      *(pc->p) == '\r' ||
	      *(pc->p) == '\n' ||
		    (*(pc->p) == '\\' && *(pc->p+1) == '\n')) {
		pc->p++;
	}

	switch(*(pc->p)) {
	case '\0':
		pc->tstart = pc->tend = pc->p;
		pc->tline = pc->linenr;
		pc->tt = JIM_TT_EOL;
		pc->eof = 1;
		break;
	case '(':
		pc->tstart = pc->tend = pc->p;
		pc->tline = pc->linenr;
		pc->tt = JIM_TT_SUBEXPR_START;
		pc->p++;
		break;
	case ')':
		pc->tstart = pc->tend = pc->p;
		pc->tline = pc->linenr;
		pc->tt = JIM_TT_SUBEXPR_END;
		pc->p++;
		break;
	case '[':
		return JimParseCmd(pc);
		break;
	case '$':
		if (JimParseVar(pc) == JIM_ERR)
			return JimParseExprOperator(pc);
		else
			return JIM_OK;
		break;
	case '-':
		if ((pc->tt == JIM_TT_NONE || pc->tt == JIM_TT_EXPR_OPERATOR) &&
		    isdigit((int)*(pc->p+1)))
			return JimParseExprNumber(pc);
		else
			return JimParseExprOperator(pc);
		break;
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9': case '.':
		return JimParseExprNumber(pc);
		break;
	case '"':
	case '{':
		/* Here it's possible to reuse the List String parsing. */
		pc->tt = JIM_TT_NONE; /* Make sure it's sensed as a new word. */
		return JimParseListStr(pc);
		break;
	default:
		return JimParseExprOperator(pc);
		break;
	}
	return JIM_OK;
}

int JimParseExprNumber(struct JimParserCtx *pc)
{
	int allowdot = 1;

	pc->tstart = pc->p;
	pc->tline = pc->linenr;
	if (*pc->p == '-') pc->p++;
	while (isdigit((int)*pc->p) || (allowdot && *pc->p == '.')) {
		if (*pc->p == '.')
			allowdot = 0;
		pc->p++;
		if (!allowdot && *pc->p == 'e' && *(pc->p+1) == '-')
			pc->p += 2;
	}
	pc->tend = pc->p-1;
	pc->tt = JIM_TT_EXPR_NUMBER;
	return JIM_OK;
}

int JimParseExprOperator(struct JimParserCtx *pc)
{
	int i;
	int bestIdx = -1, bestLen = 0;

	/* Try to get the longest match. */
	for (i = 0; i < (signed)JIM_EXPR_OPERATORS_NUM; i++) {
		char *opname = Jim_ExprOperators[i].name;
		int oplen = strlen(opname);

		if (strncmp(opname, pc->p, oplen) == 0 && oplen > bestLen) {
			bestIdx = i;
			bestLen = oplen;
		}
	}
	if (bestIdx == -1) return JIM_ERR;
	pc->tstart = pc->p;
	pc->tend = pc->p + bestLen - 1;
	pc->p += bestLen;
	pc->tline = pc->linenr;
	pc->tt = JIM_TT_EXPR_OPERATOR;
	return JIM_OK;
}

struct Jim_ExprOperator *Jim_ExprOperatorInfo(char *opname)
{
	int i;
	for (i = 0; i < (signed)JIM_EXPR_OPERATORS_NUM; i++)
		if (strcmp(opname, Jim_ExprOperators[i].name) == 0)
			return &Jim_ExprOperators[i];
	return NULL;
}

/* -----------------------------------------------------------------------------
 * Expression Object
 * ---------------------------------------------------------------------------*/
static void FreeExprInternalRep(Jim_Interp *interp, Jim_Obj *objPtr);
static void DupExprInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr);
static int SetExprFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr);

Jim_ObjType exprObjType = {
	"expression",
	FreeExprInternalRep,
	DupExprInternalRep,
	NULL,
	JIM_TYPE_REFERENCES,
};

/* Expr bytecode structure */
typedef struct ExprByteCode {
	int *opcode;	/* Integer array of opcodes. */
	Jim_Obj **obj;	/* Array of associated Jim Objects. */
	int len;	/* Bytecode length */
	int inUse;	/* Used for sharing. */
} ExprByteCode;

void FreeExprInternalRep(Jim_Interp *interp, Jim_Obj *objPtr)
{
	int i;
	ExprByteCode *expr = (void*) objPtr->internalRep.ptr;

	expr->inUse--;
	if (expr->inUse != 0) return;
	for (i = 0; i < expr->len; i++)
		Jim_DecrRefCount(interp, expr->obj[i]);
	Jim_Free(expr->opcode);
	Jim_Free(expr->obj);
	Jim_Free(expr);
}

void DupExprInternalRep(Jim_Interp *interp, Jim_Obj *srcPtr, Jim_Obj *dupPtr)
{
	interp = interp;
	srcPtr = srcPtr;

	/* Just returns an simple string. */
	dupPtr->typePtr = NULL;
}

/* Add a new instruction to an expression bytecode structure. */
static void ExprObjAddInstr(Jim_Interp *interp, ExprByteCode *expr,
		int opcode, char *str, int len)
{
	expr->opcode = Jim_Realloc(expr->opcode, sizeof(int)*(expr->len+1));
	expr->obj = Jim_Realloc(expr->obj, sizeof(Jim_Obj*)*(expr->len+1));
	expr->opcode[expr->len] = opcode;
	expr->obj[expr->len] = Jim_NewStringObjNoAlloc(interp, str, len);
	Jim_IncrRefCount(expr->obj[expr->len]);
	expr->len++;
}

/* Check if an expr program looks correct. */
static int ExprCheckCorrectness(ExprByteCode *expr)
{
	int i;
	int stacklen = 0;

	/* Try to check if there are stack underflows,
	 * and make sure at the end of the program there is
	 * a single result on the stack. */
	for (i = 0; i < expr->len; i++) {
		switch(expr->opcode[i]) {
		case JIM_EXPROP_NUMBER:
		case JIM_EXPROP_STRING:
		case JIM_EXPROP_VARIABLE:
		case JIM_EXPROP_DICTSUGAR:
		case JIM_EXPROP_COMMAND:
			stacklen++;
			break;
		case JIM_EXPROP_NOT:
		case JIM_EXPROP_BITNOT:
		case JIM_EXPROP_UNARYMINUS:
		case JIM_EXPROP_UNARYPLUS:
			/* Unary operations */
			if (stacklen < 1) return JIM_ERR;
			break;
		case JIM_EXPROP_ADD:
		case JIM_EXPROP_SUB:
		case JIM_EXPROP_MUL:
		case JIM_EXPROP_DIV:
		case JIM_EXPROP_MOD:
		case JIM_EXPROP_LT:
		case JIM_EXPROP_GT:
		case JIM_EXPROP_LTE:
		case JIM_EXPROP_GTE:
		case JIM_EXPROP_ROTL:
		case JIM_EXPROP_ROTR:
		case JIM_EXPROP_LSHIFT:
		case JIM_EXPROP_RSHIFT:
		case JIM_EXPROP_NUMEQ:
		case JIM_EXPROP_NUMNE:
		case JIM_EXPROP_STREQ:
		case JIM_EXPROP_STRNE:
		case JIM_EXPROP_BITAND:
		case JIM_EXPROP_BITXOR:
		case JIM_EXPROP_BITOR:
		case JIM_EXPROP_LOGICAND:
		case JIM_EXPROP_LOGICOR:
			/* binary operations */
			if (stacklen < 2) return JIM_ERR;
			stacklen--;
			break;
		default:
			Jim_Panic("Default opcode reached ExprCheckCorrectness");
			break;
		}
	}
	if (stacklen != 1) return JIM_ERR;
	return JIM_OK;
}

static void ExprShareLiterals(Jim_Interp *interp, ExprByteCode *expr,
		ScriptObj *topLevelScript)
{
	int i;

	for (i = 0; i < expr->len; i++) {
		Jim_Obj *foundObjPtr;

		if (expr->obj[i] == NULL) continue;
		foundObjPtr = ScriptSearchLiteral(interp, topLevelScript,
				expr->obj[i]);
		if (foundObjPtr != NULL) {
			Jim_IncrRefCount(foundObjPtr);
			Jim_DecrRefCount(interp, expr->obj[i]);
			expr->obj[i] = foundObjPtr;
		}
	}
}

/* This method takes the string representation of an expression
 * and generates a program for the Expr's stack-based VM. */
int SetExprFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr)
{
	char *exprText = Jim_GetString(objPtr, NULL);
	struct JimParserCtx parser;
	int i, shareLiterals;
	ExprByteCode *expr = Jim_Alloc(sizeof(*expr));
	Jim_Stack stack;
	Jim_ExprOperator *op;

	/* Perform literal sharing with the current procedure
	 * running only if this expression appears to be not generated
	 * at runtime. */
	shareLiterals = objPtr->typePtr == &sourceObjType;

	expr->opcode = NULL;
	expr->obj = NULL;
	expr->len = 0;
	expr->inUse = 1;

	Jim_InitStack(&stack);
	JimParserInit(&parser, exprText, 1);
	while(!JimParserEof(&parser)) {
		char *token;
		int len, type;

		if (JimParseExpression(&parser) != JIM_OK) {
			Jim_SetResultString(interp,
				"Syntax error in expression", -1);
			goto err;
		}
		token = JimParserGetToken(&parser, &len, &type, NULL);
		if (type == JIM_TT_EOL) {
			Jim_Free(token);
			break;
		}
		switch(type) {
		case JIM_TT_STR:
		case JIM_TT_ESC:
			ExprObjAddInstr(interp, expr,
					JIM_EXPROP_STRING, token, len);
			break;
		case JIM_TT_VAR:
			ExprObjAddInstr(interp, expr,
					JIM_EXPROP_VARIABLE, token, len);
			break;
		case JIM_TT_DICTSUGAR:
			ExprObjAddInstr(interp, expr,
					JIM_EXPROP_DICTSUGAR, token, len);
			break;
		case JIM_TT_CMD:
			ExprObjAddInstr(interp, expr,
					JIM_EXPROP_COMMAND, token, len);
			break;
		case JIM_TT_EXPR_NUMBER:
			ExprObjAddInstr(interp, expr,
					JIM_EXPROP_NUMBER, token, len);
			break;
		case JIM_TT_EXPR_OPERATOR:
			op = Jim_ExprOperatorInfo(token);
			while(1) {
				Jim_ExprOperator *stackTopOp;

				if (Jim_StackPeek(&stack) != NULL) {
					stackTopOp =
					  Jim_ExprOperatorInfo(Jim_StackPeek(&stack));
				} else {
					stackTopOp = NULL;
				}
				if (Jim_StackLen(&stack) &&
				    op->arity != 1 &&
				    stackTopOp &&
				    stackTopOp->precedence >= op->precedence)
				{
					ExprObjAddInstr(interp, expr,
						stackTopOp->opcode,
						Jim_StackPeek(&stack), -1);
					Jim_StackPop(&stack);
				} else {
					break;
				}
			}
			Jim_StackPush(&stack, token);
			break;
		case JIM_TT_SUBEXPR_START:
			Jim_StackPush(&stack, Jim_StrDup("("));
			Jim_Free(token);
			break;
		case JIM_TT_SUBEXPR_END:
			{
				int found = 0;
				while(Jim_StackLen(&stack)) {
					char *opstr = Jim_StackPop(&stack);
					if (!strcmp(opstr, "(")) {
						Jim_Free(opstr);
						found = 1;
						break;
					}
					op = Jim_ExprOperatorInfo(opstr);
					ExprObjAddInstr(interp, expr,
							op->opcode, opstr, -1);
				}
				if (!found) {
					Jim_SetResultString(interp,
					  "Unexpected close parenthesis", -1);
					goto err;
				}
			}
			Jim_Free(token);
			break;
		default:
			Jim_Panic("Default reached in SetExprFromAny()");
			break;
		}
	}
	while (Jim_StackLen(&stack)) {
		char *opstr = Jim_StackPop(&stack);
		op = Jim_ExprOperatorInfo(opstr);
		if (op == NULL && !strcmp(opstr, "(")) {
			Jim_Free(opstr);
			Jim_SetResultString(interp,
				"Missing close parenthesis", -1);
			goto err;
		}
		ExprObjAddInstr(interp, expr, op->opcode,
			opstr, -1);
	}
	/* Check program correctness. */
	if (ExprCheckCorrectness(expr) != JIM_OK) {
		Jim_SetResultString(interp,
			"Invalid expression", -1);
		goto err;
	}

	/* Free the stack used for the compilation. */
	Jim_FreeStackElements(&stack, Jim_Free);
	Jim_FreeStack(&stack);

	/* Perform literal sharing */
	if (shareLiterals && interp->framePtr->procBodyObjPtr) {
		Jim_Obj *bodyObjPtr = interp->framePtr->procBodyObjPtr;
		if (bodyObjPtr->typePtr == &scriptObjType) {
			ScriptObj *bodyScript =
				bodyObjPtr->internalRep.ptr;
			ExprShareLiterals(interp, expr, bodyScript);
		}
	}

	/* Free the old internal rep and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	Jim_SetIntRepPtr(objPtr, expr);
	objPtr->typePtr = &exprObjType;
	return JIM_OK;

err:	/* we jump here on syntax/compile errors. */
	Jim_FreeStackElements(&stack, Jim_Free);
	Jim_FreeStack(&stack);
	Jim_Free(expr->opcode);
	for (i = 0; i < expr->len; i++) {
		Jim_DecrRefCount(interp,expr->obj[i]);
	}
	Jim_Free(expr->obj);
	Jim_Free(expr);
	return JIM_ERR;
}

ExprByteCode *Jim_GetExpression(Jim_Interp *interp, Jim_Obj *objPtr)
{
	if (objPtr->typePtr != &exprObjType) {
		if (SetExprFromAny(interp, objPtr) != JIM_OK)
			return NULL;
	}
	return (ExprByteCode*) Jim_GetIntRepPtr(objPtr);
}

/* -----------------------------------------------------------------------------
 * Expressions evaluation.
 * Jim uses a specialized stack-based virtual machine for expressions,
 * that takes advantage of the fact that expr's operators
 * can't be redefined.
 *
 * Jim_EvalExpression() uses the bytecode compiled by
 * SetExprFromAny() method of the "expression" object.
 *
 * On success a Tcl Object containing the result of the evaluation
 * is stored into expResultPtrPtr (having refcount of 1), and JIM_OK is
 * returned.
 * On error the function returns a retcode != to JIM_OK and set a suitable
 * error on the interp.
 * ---------------------------------------------------------------------------*/
#define JIM_EE_STATICSTACK_LEN 10

int Jim_EvalExpression(Jim_Interp *interp, Jim_Obj *exprObjPtr,
		Jim_Obj **exprResultPtrPtr)
{
	ExprByteCode *expr;
	Jim_Obj **stack, *staticStack[JIM_EE_STATICSTACK_LEN];
	int stacklen = 0, i, error = 0, errRetCode = JIM_ERR;

	Jim_IncrRefCount(exprObjPtr);
	expr = Jim_GetExpression(interp, exprObjPtr);
	if (!expr) {
		Jim_DecrRefCount(interp, exprObjPtr);
		return JIM_ERR; /* error in expression. */
	}
	/* In order to avoid that the internal repr gets freed due to
	 * shimmering of the exprObjPtr's object, we make the internal rep
	 * shared. */
	expr->inUse++;

	/* The stack-based expr VM itself */

	/* Stack allocation. Expr programs have the feature that
	 * a program of length N can't require a stack longer than
	 * N. */
	if (expr->len > JIM_EE_STATICSTACK_LEN)
		stack = Jim_Alloc(sizeof(Jim_Obj*)*expr->len);
	else
		stack = staticStack;

	/* Execute every istruction */
	for (i = 0; i < expr->len; i++) {
		Jim_Obj *A, *B, *objPtr;
		jim_wide wA, wB, wC;
		double dA, dB, dC;
		char *sA, *sB;
		int Alen, Blen, retcode;

		switch(expr->opcode[i]) {
		case JIM_EXPROP_NUMBER:
		case JIM_EXPROP_STRING:
			stack[stacklen++] = expr->obj[i];
			Jim_IncrRefCount(expr->obj[i]);
			break;
		case JIM_EXPROP_VARIABLE:
			objPtr = Jim_GetVariable(interp, expr->obj[i],
					JIM_ERRMSG);
			if (objPtr == NULL) {
				error = 1;
				goto err;
			}
			stack[stacklen++] = objPtr;
			Jim_IncrRefCount(objPtr);
			break;
		case JIM_EXPROP_DICTSUGAR:
			objPtr = Jim_ExpandDictSugar(interp, expr->obj[i]);
			if (objPtr == NULL) {
				error = 1;
				goto err;
			}
			stack[stacklen++] = objPtr;
			Jim_IncrRefCount(objPtr);
			break;
		case JIM_EXPROP_COMMAND:
			if ((retcode = Jim_EvalObj(interp, expr->obj[i]))
					!= JIM_OK)
			{
				error = 1;
				errRetCode = retcode;
				goto err;
			}
			stack[stacklen++] = interp->result;
			Jim_IncrRefCount(interp->result);
			break;
		case JIM_EXPROP_ADD:
		case JIM_EXPROP_SUB:
		case JIM_EXPROP_MUL:
		case JIM_EXPROP_DIV:
		case JIM_EXPROP_MOD:
		case JIM_EXPROP_LT:
		case JIM_EXPROP_GT:
		case JIM_EXPROP_LTE:
		case JIM_EXPROP_GTE:
		case JIM_EXPROP_ROTL:
		case JIM_EXPROP_ROTR:
		case JIM_EXPROP_LSHIFT:
		case JIM_EXPROP_RSHIFT:
		case JIM_EXPROP_NUMEQ:
		case JIM_EXPROP_NUMNE:
		case JIM_EXPROP_BITAND:
		case JIM_EXPROP_BITXOR:
		case JIM_EXPROP_BITOR:
		case JIM_EXPROP_LOGICAND:
		case JIM_EXPROP_LOGICOR:
			/* Note that there isn't to increment the
			 * refcount of objects. the references are moved
			 * from stack to A and B. */
			B = stack[--stacklen];
			A = stack[--stacklen];

			/* --- Integer --- */
			if ((A->typePtr == &doubleObjType && !A->bytes) ||
			    (B->typePtr == &doubleObjType && !B->bytes) ||
			    Jim_GetWide(interp, A, &wA) != JIM_OK ||
			    Jim_GetWide(interp, B, &wB) != JIM_OK) {
				goto trydouble;
			}
			Jim_DecrRefCount(interp, A);
			Jim_DecrRefCount(interp, B);
			switch(expr->opcode[i]) {
			case JIM_EXPROP_ADD: wC = wA+wB; break;
			case JIM_EXPROP_SUB: wC = wA-wB; break;
			case JIM_EXPROP_MUL: wC = wA*wB; break;
			case JIM_EXPROP_LT: wC = wA<wB; break;
			case JIM_EXPROP_GT: wC = wA>wB; break;
			case JIM_EXPROP_LTE: wC = wA<=wB; break;
			case JIM_EXPROP_GTE: wC = wA>=wB; break;
			case JIM_EXPROP_LSHIFT: wC = wA<<wB; break;
			case JIM_EXPROP_RSHIFT: wC = wA>>wB; break;
			case JIM_EXPROP_NUMEQ: wC = wA==wB; break;
			case JIM_EXPROP_NUMNE: wC = wA!=wB; break;
			case JIM_EXPROP_BITAND: wC = wA&wB; break;
			case JIM_EXPROP_BITXOR: wC = wA^wB; break;
			case JIM_EXPROP_BITOR: wC = wA|wB; break;
			case JIM_EXPROP_LOGICAND: wC = wA&&wB; break;
			case JIM_EXPROP_LOGICOR: wC = wA||wB; break;
			case JIM_EXPROP_DIV:
				if (wB == 0) goto divbyzero;
				wC = wA/wB;
				break;
			case JIM_EXPROP_MOD:
				if (wB == 0) goto divbyzero;
				wC = wA%wB;
				break;
			case JIM_EXPROP_ROTL: {
			    unsigned long uA = (unsigned jim_wide)wA&0xFFFFFFFF;
			    const unsigned int S = sizeof(unsigned long) * 8;
			    wC = (jim_wide)((uA<<wB)|(uA>>(S-wB)));
			    break;
			}
			case JIM_EXPROP_ROTR: {
			    unsigned long uA = (unsigned jim_wide)wA&0xFFFFFFFF;
			    const unsigned int S = sizeof(unsigned long) * 8;
			    wC = (jim_wide)((uA>>wB)|(uA<<(S-wB)));
			    break;
			}

			default:
				wC = 0; /* avoid gcc warning */
				break;
			}
			stack[stacklen] = Jim_NewIntObj(interp, wC);
			Jim_IncrRefCount(stack[stacklen]);
			stacklen++;
			break;
trydouble:
			/* --- Double --- */
			if (Jim_GetDouble(interp, A, &dA) != JIM_OK ||
			    Jim_GetDouble(interp, B, &dB) != JIM_OK) {
				Jim_DecrRefCount(interp, A);
				Jim_DecrRefCount(interp, B);
				error = 1;
				goto err;
			}
			Jim_DecrRefCount(interp, A);
			Jim_DecrRefCount(interp, B);
			switch(expr->opcode[i]) {
			case JIM_EXPROP_ROTL:
			case JIM_EXPROP_ROTR:
			case JIM_EXPROP_LSHIFT:
			case JIM_EXPROP_RSHIFT:
			case JIM_EXPROP_BITAND:
			case JIM_EXPROP_BITXOR:
			case JIM_EXPROP_BITOR:
			case JIM_EXPROP_MOD:
				Jim_SetResultString(interp,
					"Got floating-point value where "
					"integer was expected", -1);
				error = 1;
				goto err;
				break;
			case JIM_EXPROP_ADD: dC = dA+dB; break;
			case JIM_EXPROP_SUB: dC = dA-dB; break;
			case JIM_EXPROP_MUL: dC = dA*dB; break;
			case JIM_EXPROP_LT: dC = dA<dB; break;
			case JIM_EXPROP_GT: dC = dA>dB; break;
			case JIM_EXPROP_LTE: dC = dA<=dB; break;
			case JIM_EXPROP_GTE: dC = dA>=dB; break;
			case JIM_EXPROP_NUMEQ: dC = dA==dB; break;
			case JIM_EXPROP_NUMNE: dC = dA!=dB; break;
			case JIM_EXPROP_LOGICAND: dC = dA&&dB; break;
			case JIM_EXPROP_LOGICOR: dC = dA||dB; break;
			case JIM_EXPROP_DIV:
				if (dB == 0) goto divbyzero;
				dC = dA/dB;
				break;
			default:
				dC = 0; /* avoid gcc warning */
				break;
			}
			stack[stacklen] = Jim_NewDoubleObj(interp, dC);
			Jim_IncrRefCount(stack[stacklen]);
			stacklen++;
			break;
		case JIM_EXPROP_STREQ:
		case JIM_EXPROP_STRNE:
			B = stack[--stacklen];
			A = stack[--stacklen];
			sA = Jim_GetString(A, &Alen);
			sB = Jim_GetString(B, &Blen);
			switch(expr->opcode[i]) {
			case JIM_EXPROP_STREQ:
				if (Alen == Blen &&
				    memcmp(sA, sB, Alen) ==0)
					wC = 1;
				else
					wC = 0;
				break;
			case JIM_EXPROP_STRNE:
				if (Alen != Blen ||
				    memcmp(sA, sB, Alen) != 0)
					wC = 1;
				else
					wC = 0;
				break;
			default:
				wC = 0; /* avoid gcc warning */
				break;
			}
			Jim_DecrRefCount(interp, A);
			Jim_DecrRefCount(interp, B);
			stack[stacklen] = Jim_NewIntObj(interp, wC);
			Jim_IncrRefCount(stack[stacklen]);
			stacklen++;
			break;
		case JIM_EXPROP_NOT:
		case JIM_EXPROP_BITNOT:
			/* Note that there isn't to increment the
			 * refcount of objects. the references are moved
			 * from stack to A and B. */
			A = stack[--stacklen];

			/* --- Integer --- */
			if ((A->typePtr == &doubleObjType && !A->bytes) ||
			    Jim_GetWide(interp, A, &wA) != JIM_OK) {
				goto trydouble_unary;
			}
			Jim_DecrRefCount(interp, A);
			switch(expr->opcode[i]) {
			case JIM_EXPROP_NOT: wC = !wA; break;
			case JIM_EXPROP_BITNOT: wC = ~wA; break;
			default:
				wC = 0; /* avoid gcc warning */
				break;
			}
			stack[stacklen] = Jim_NewIntObj(interp, wC);
			Jim_IncrRefCount(stack[stacklen]);
			stacklen++;
			break;
trydouble_unary:
			/* --- Double --- */
			if (Jim_GetDouble(interp, A, &dA) != JIM_OK) {
				Jim_DecrRefCount(interp, A);
				error = 1;
				goto err;
			}
			Jim_DecrRefCount(interp, A);
			switch(expr->opcode[i]) {
			case JIM_EXPROP_NOT: dC = !dA; break;
			case JIM_EXPROP_BITNOT:
				Jim_SetResultString(interp,
					"Got floating-point value where "
					"integer was expected", -1);
				error = 1;
				goto err;
				break;
			default:
				dC = 0; /* avoid gcc warning */
				break;
			}
			stack[stacklen] = Jim_NewDoubleObj(interp, dC);
			Jim_IncrRefCount(stack[stacklen]);
			stacklen++;
			break;
		default:
			Jim_Panic("Default opcode reached Jim_EvalExpression");
			break;
		}
	}
err:
	/* There is no need to decerement the inUse field because
	 * this reference is transfered back into the exprObjPtr. */
	Jim_FreeIntRep(interp, exprObjPtr);
	exprObjPtr->typePtr = &exprObjType;
	Jim_SetIntRepPtr(exprObjPtr, expr);
	Jim_DecrRefCount(interp, exprObjPtr);
	if (!error) {
		*exprResultPtrPtr = stack[0];
		Jim_IncrRefCount(stack[0]);
		errRetCode = JIM_OK;
	}
	for (i = 0; i < stacklen; i++) {
		Jim_DecrRefCount(interp, stack[i]);
	}
	if (stack != staticStack)
		Jim_Free(stack);
	return errRetCode;
divbyzero:
	error = 1;
	Jim_SetResultString(interp, "Division by zero", -1);
	goto err;
}

int Jim_GetBoolFromExpr(Jim_Interp *interp, Jim_Obj *exprObjPtr, int *boolPtr)
{
	int retcode;
	jim_wide wideValue;
	double doubleValue;
	Jim_Obj *exprResultPtr;

	retcode = Jim_EvalExpression(interp, exprObjPtr, &exprResultPtr);
	if (retcode != JIM_OK)
		return retcode;
	if (Jim_GetWide(interp, exprResultPtr, &wideValue) != JIM_OK) {
		if (Jim_GetDouble(interp, exprResultPtr, &doubleValue) != JIM_OK)
		{
			Jim_DecrRefCount(interp, exprResultPtr);
			return JIM_ERR;
		} else {
			Jim_DecrRefCount(interp, exprResultPtr);
			*boolPtr = doubleValue != 0;
			return JIM_OK;
		}
	}
	Jim_DecrRefCount(interp, exprResultPtr);
	*boolPtr = wideValue != 0;
	return JIM_OK;
}

/* -----------------------------------------------------------------------------
 * Dynamic libraries support (WIN32 not supported)
 * ---------------------------------------------------------------------------*/

#ifndef WIN32
#include <dlfcn.h>
#define JIM_LIBPATH_LEN 1024
int Jim_LoadLibrary(Jim_Interp *interp, char *pathName)
{
	Jim_Obj *libPathObjPtr;
	int prefixc, i;
	void *handle;
	int (*onload)(Jim_Interp *interp);

	libPathObjPtr = Jim_GetVariableString(interp, "jim::libpath", JIM_NONE);
	if (libPathObjPtr == NULL) {
		prefixc = 0;
		libPathObjPtr = NULL;
	} else {
		Jim_IncrRefCount(libPathObjPtr);
		Jim_ListLength(interp, libPathObjPtr, &prefixc);
	}

	for (i = -1; i < prefixc; i++) {
		if (i < 0) {
			handle = dlopen(pathName, RTLD_LAZY);
		} else {
			FILE *fp;
			char buf[JIM_LIBPATH_LEN];
			char *prefix;
			int prefixlen;
			Jim_Obj *prefixObjPtr;
			
			buf[0] = '\0';
			if (Jim_ListIndex(interp, libPathObjPtr, i,
					&prefixObjPtr, JIM_NONE) != JIM_OK)
				continue;
			prefix = Jim_GetString(prefixObjPtr, NULL);
			prefixlen = strlen(prefix);
			if (prefixlen+strlen(pathName)+1 >=
					JIM_LIBPATH_LEN)
				continue;
			if (prefixlen && prefix[prefixlen-1] == '/')
				sprintf(buf, "%s%s", prefix, pathName);
			else
				sprintf(buf, "%s/%s", prefix, pathName);
			fp = fopen(buf, "r");
			if (fp == NULL)
				continue;
			fclose(fp);
			handle = dlopen(buf, RTLD_LAZY);
		}
		if (handle == NULL) {
			Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
			Jim_AppendStrings(interp, Jim_GetResult(interp),
				"error loading extension \"", pathName,
				"\": ", dlerror(), NULL);
			if (i < 0)
				continue;
			goto err;
		}
		if ((onload = dlsym(handle, "Jim_OnLoad")) == NULL) {
			Jim_SetResultString(interp,
					"No Jim_OnLoad symbol found on extension", -1);
			goto err;
		}
		if (onload(interp) == JIM_ERR) {
			dlclose(handle);
			goto err;
		}
		if (libPathObjPtr != NULL)
			Jim_DecrRefCount(interp, libPathObjPtr);
		return JIM_OK;
	}
err:
	if (libPathObjPtr != NULL)
		Jim_DecrRefCount(interp, libPathObjPtr);
	return JIM_ERR;
}
#else
int Jim_LoadLibrary(Jim_Interp *interp, char *pathName)
{
	pathName = pathName;
	Jim_SetResultString(interp,
			"Dynamic libraries not supported under WIN32", -1);
	return JIM_ERR;
}
#endif /* ! WIN32 */

/* -----------------------------------------------------------------------------
 * Eval
 * ---------------------------------------------------------------------------*/
#define JIM_EVAL_SARGV_LEN 8 /* static arguments vector length */
#define JIM_EVAL_SINTV_LEN 8 /* static interpolation vector length */

static int Jim_CallProcedure(Jim_Interp *interp, Jim_Cmd *cmd, int argc,
		Jim_Obj **argv);

/* Hanlde class to the [unknown] command */
static int Jim_Unknown(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	Jim_Obj **v, *sv[JIM_EVAL_SARGV_LEN];
	int retCode;

	/* If the [unknown] command does not exists returns
	 * just now */
	if (Jim_GetCommand(interp, interp->unknown, JIM_NONE) == NULL)
		return JIM_ERR;

	/* The object interp->unknown just contains
	 * the "unknown" string, it is used in order to
	 * avoid to lookup the unknown command every time
	 * but instread to cache the result. */
	if (argc+1 <= JIM_EVAL_SARGV_LEN)
		v = sv;
	else
		v = Jim_Alloc(sizeof(Jim_Obj*)*argc+1);
	/* Make a copy of the arguments vector, but shifted on
	 * the right of one position. The command name of the
	 * command will be instead the first argument of the
	 * [unknonw] call. */
	memcpy(v+1, argv, sizeof(Jim_Obj*)*argc);
	v[0] = interp->unknown;
	/* Call it */
	retCode = Jim_EvalObjVector(interp, argc+1, v);
	/* Clean up */
	if (v != sv)
		Jim_Free(v);
	return retCode;
}

/* Eval the object vector 'objv' composed of 'objc' elements.
 * Every element is used as single argument.
 * Jim_EvalObj() will call this function every time its object
 * argument is of "list" type, with no string representation.
 *
 * This is possible because the string representation of a
 * list object generated by the UpdateStringOfList is made
 * in a way that ensures that every list element is a different
 * command argument. */
int Jim_EvalObjVector(Jim_Interp *interp, int objc, Jim_Obj **objv)
{
	int i, retcode;
	Jim_Cmd *cmdPtr;

	/* Incr refcount of arguments. */
	for (i = 0; i < objc; i++)
		Jim_IncrRefCount(objv[i]);
	/* Command lookup */
	cmdPtr = Jim_GetCommand(interp, objv[0], JIM_ERRMSG);
	if (cmdPtr == NULL) {
		retcode = Jim_Unknown(interp, objc, objv);
	} else {
		/* Call it -- Make sure result is an empty object. */
		Jim_SetEmptyResult(interp);
		if (cmdPtr->cmdProc) {
			interp->cmdPrivData = cmdPtr->privData;
			retcode = cmdPtr->cmdProc(interp, objc, objv);
		} else {
			retcode = Jim_CallProcedure(interp, cmdPtr, objc, objv);
			if (retcode == JIM_ERR) {
				Jim_AppendStackTrace(interp,
					Jim_GetString(objv[0], NULL),
					"?", 1);
			}
		}
	}
	/* Decr refcount of arguments and return the retcode */
	for (i = 0; i < objc; i++)
		Jim_DecrRefCount(interp, objv[i]);
	return retcode;
}

/* Interpolate the given tokens into a unique Jim_Obj returned by reference
 * via *objPtrPtr. This function is only called by Jim_EvalObj().
 * The returned object has refcount = 0. */
int Jim_InterpolateTokens(Jim_Interp *interp, ScriptToken *token,
		int tokens, Jim_Obj **objPtrPtr)
{
	int totlen = 0, i, retcode;
	Jim_Obj **intv;
	Jim_Obj *sintv[JIM_EVAL_SINTV_LEN];
	Jim_Obj *objPtr;
	char *s;

	if (tokens <= JIM_EVAL_SINTV_LEN)
		intv = sintv;
	else
		intv = Jim_Alloc(sizeof(Jim_Obj*)*
				tokens);
	/* Compute every token forming the argument
	 * in the intv objects vector. */
	for (i = 0; i < tokens; i++) {
		switch(token[i].type) {
		case JIM_TT_ESC:
		case JIM_TT_STR:
			intv[i] = token[i].objPtr;
			break;
		case JIM_TT_VAR:
			intv[i] = Jim_GetVariable(
					interp,
					token[i].objPtr,
					JIM_ERRMSG);
			if (!intv[i]) {
				retcode = JIM_ERR;
				goto err;
			}
			break;
		case JIM_TT_CMD:
			retcode = Jim_EvalObj(interp,
					token[i].objPtr);
			if (retcode != JIM_OK)
				goto err;
			intv[i] = Jim_GetResult(interp);
			break;
		default:
			Jim_Panic(
			  "default token type reached "
			  "in Jim_EvalObj().");
			break;
		}
		Jim_IncrRefCount(intv[i]);
		/* Make sure there is a valid
		 * string rep, and add the string
		 * length to the total legnth. */
		Jim_GetString(intv[i], NULL);
		totlen += intv[i]->length;
	}
	/* Concatenate every token in an unique
	 * object. */
	objPtr = Jim_NewStringObjNoAlloc(interp,
			NULL, 0);
	s = objPtr->bytes = Jim_Alloc(totlen+1);
	objPtr->length = totlen;
	for (i = 0; i < tokens; i++) {
		memcpy(s, intv[i]->bytes,
				intv[i]->length);
		s += intv[i]->length;
		Jim_DecrRefCount(interp, intv[i]);
	}
	objPtr->bytes[totlen] = '\0';
	/* Free the intv vector if not static. */
	if (tokens > JIM_EVAL_SINTV_LEN)
		Jim_Free(intv);
	*objPtrPtr = objPtr;
	return JIM_OK;
err:
	i--;
	for (; i >= 0; i--)
		Jim_DecrRefCount(interp, intv[i]);
	if (tokens > JIM_EVAL_SINTV_LEN)
		Jim_Free(intv);
	return retcode;
}

/* Helper of Jim_EvalObj() to perform argument expansion.
 * Basically this function append an argument to 'argv'
 * (and increments argc by reference accordingly), performing
 * expansion of the list object if 'expand' is non-zero, or
 * just adding objPtr to argv if 'expand' is zero. */
void Jim_ExpandArgument(Jim_Interp *interp, Jim_Obj ***argv,
		int *argcPtr, int expand, Jim_Obj *objPtr)
{
	if (!expand) {
		(*argv) = Jim_Realloc(*argv, sizeof(Jim_Obj*)*((*argcPtr)+1));
		/* refcount of objPtr not incremented because
		 * we are actually transfering a reference from
		 * the old 'argv' to the expanded one. */
		(*argv)[*argcPtr] = objPtr;
		(*argcPtr)++;
	} else {
		int len, i;

		Jim_ListLength(interp, objPtr, &len);
		(*argv) = Jim_Realloc(*argv, sizeof(Jim_Obj*)*((*argcPtr)+len));
		for (i = 0; i < len; i++) {
			(*argv)[*argcPtr] = objPtr->internalRep.listValue.ele[i];
			Jim_IncrRefCount(objPtr->internalRep.listValue.ele[i]);
			(*argcPtr)++;
		}
		/* The original object reference is no longer needed,
		 * after the expansion it is no longer present on
		 * the argument vector, but the single elements are
		 * in its place. */
		Jim_DecrRefCount(interp, objPtr);
	}
}

int Jim_EvalObj(Jim_Interp *interp, Jim_Obj *scriptObjPtr)
{
	int i, j = 0, len;
	ScriptObj *script;
	ScriptToken *token;
	int *cs; /* command structure array */
	int retcode = JIM_OK;
	Jim_Obj *sargv[JIM_EVAL_SARGV_LEN], **argv = NULL, *tmpObjPtr;

	interp->errorFlag = 0;

	/* If the object is of type "list" and there is no
	 * string representation for this object, we can call
	 * a specialized version of Jim_EvalObj() */
	if (scriptObjPtr->typePtr == &listObjType &&
	    scriptObjPtr->internalRep.listValue.len &&
	    scriptObjPtr->bytes == NULL) {
		Jim_IncrRefCount(scriptObjPtr);
		retcode = Jim_EvalObjVector(interp,
				scriptObjPtr->internalRep.listValue.len,
				scriptObjPtr->internalRep.listValue.ele);
		Jim_DecrRefCount(interp, scriptObjPtr);
		return retcode;
	}

	Jim_IncrRefCount(scriptObjPtr); /* Make sure it's shared. */
	script = Jim_GetScript(interp, scriptObjPtr);
	/* Now we have to make sure the internal repr will not be
	 * freed on shimmering.
	 *
	 * Think for example to this:
	 *
	 * set x {llength $x; ... some more code ...}; eval $x
	 *
	 * In order to preserve the internal rep, we increment the
	 * inUse field of the script internal rep structure. */
	script->inUse++;

	token = script->token;
	len = script->len;
	cs = script->cmdStruct;
	i = 0; /* 'i' is the current token index. */

	/* Reset the interpreter result. This is useful to
	 * return the emtpy result in the case of empty program. */
	Jim_SetEmptyResult(interp);

	/* Execute every command sequentially, returns on
	 * error (i.e. if a command does not return JIM_OK) */
	while (i < len) {
		int expand = 0;
		int argc = *cs++; /* Get the number of arguments */
		Jim_Cmd *cmd;

		/* Set the expand flag if needed. */
		if (argc == -1) {
			expand++;
			argc = *cs++;
		}
		/* Allocate the arguments vector */
		if (argc <= JIM_EVAL_SARGV_LEN)
			argv = sargv;
		else
			argv = Jim_Alloc(sizeof(Jim_Obj*)*argc);
		/* Populate the arguments objects. */
		for (j = 0; j < argc; j++) {
			int tokens = *cs++;

			/* tokens is negative if expansion is needed.
			 * for this argument. */
			if (tokens < 0) {
				tokens = (-tokens)-1;
				i++;
			}
			if (tokens == 1) {
				/* Fast path if the token does not
				 * need interpolation */
				switch(token[i].type) {
				case JIM_TT_ESC:
				case JIM_TT_STR:
					argv[j] = token[i].objPtr;
					break;
				case JIM_TT_VAR:
					tmpObjPtr = Jim_GetVariable(
							interp,
							token[i].objPtr,
							JIM_ERRMSG);
					if (!tmpObjPtr) {
						retcode = JIM_ERR;
						goto err;
					}
					argv[j] = tmpObjPtr;
					break;
				case JIM_TT_DICTSUGAR:
					tmpObjPtr = Jim_ExpandDictSugar(
							interp,
							token[i].objPtr);
					if (!tmpObjPtr) {
						retcode = JIM_ERR;
						goto err;
					}
					argv[j] = tmpObjPtr;
					break;
				case JIM_TT_CMD:
					retcode = Jim_EvalObj(interp,
							token[i].objPtr);
					if (retcode != JIM_OK) {
						goto err;
					}
					argv[j] = Jim_GetResult(interp);
					break;
				default:
					Jim_Panic(
					  "default token type reached "
					  "in Jim_EvalObj().");
					break;
				}
				Jim_IncrRefCount(argv[j]);
				i += 2;
			} else {
				/* For interpolation we call an helper
				 * function doing the work for us. */
				if ((retcode = Jim_InterpolateTokens(interp,
						token+i, tokens, &tmpObjPtr)) !=
						JIM_OK)
				{
					goto err;
				}
				argv[j] = tmpObjPtr;
				Jim_IncrRefCount(argv[j]);
				i += tokens+1;
			}
		}
		/* Handle {expand} expansion */
		if (expand) {
			int *ecs = cs - argc;
			int eargc = 0;
			Jim_Obj **eargv = NULL;

			for (j = 0; j < argc; j++) {
				Jim_ExpandArgument(
						interp, &eargv, &eargc,
						ecs[j] < 0, argv[j]);
			}
			if (argv != sargv)
				Jim_Free(argv);
			argc = eargc;
			argv = eargv;
			j = argc;
			if (argc == 0) {
				/* Nothing to do with zero args. */
				Jim_Free(eargv);
				continue;
			}
		}
		/* Lookup the command to call */
		cmd = Jim_GetCommand(interp, argv[0], JIM_ERRMSG);
		if (cmd != NULL) {
			/* Call it -- Make sure result is an empty object. */
			Jim_SetEmptyResult(interp);
			if (cmd->cmdProc) {
				interp->cmdPrivData = cmd->privData;
				retcode = cmd->cmdProc(interp, argc, argv);
			} else {
				retcode = Jim_CallProcedure(interp, cmd,
						argc, argv);
				if (retcode == JIM_ERR) {
					Jim_AppendStackTrace(interp,
						Jim_GetString(argv[0], NULL),
						script->fileName,
						token[i-argc*2].linenr);
				}
			}
		} else {
			/* Call [unknown] */
			retcode = Jim_Unknown(interp, argc, argv);
		}
		if (retcode != JIM_OK) {
			i -= argc*2; /* point to the command name. */
			goto err;
		}
		/* Decrement the arguments count */
		for (j = 0; j < argc; j++) {
			Jim_DecrRefCount(interp, argv[j]);
		}

		if (argv != sargv) {
			Jim_Free(argv);
			argv = NULL;
		}
	}
	/* Note that we don't have to decrement inUse, because the
	 * following code transfers our use of the reference again to
	 * the script object. */
	j = 0; /* on normal termination, the argv array is already
		  Jim_DecrRefCount-ed. */
err:
	if (retcode == JIM_ERR && !interp->errorFlag) {
		interp->errorFlag = 1;
		Jim_SetErrorFileName(interp, script->fileName);
		Jim_SetErrorLineNumber(interp, token[i].linenr);
		Jim_ResetStackTrace(interp);
	}
	Jim_FreeIntRep(interp, scriptObjPtr);
	scriptObjPtr->typePtr = &scriptObjType;
	Jim_SetIntRepPtr(scriptObjPtr, script);
	Jim_DecrRefCount(interp, scriptObjPtr);
	for (i = 0; i < j; i++) {
		Jim_DecrRefCount(interp, argv[i]);
	}
	if (argv != sargv)
		Jim_Free(argv);
	return retcode;
}

/* Call a procedure implemented in Tcl.
 * It's possible to speed-up a lot this function, currently
 * the callframes are not cached, but allocated and
 * destroied every time. What is expecially costly is
 * to create/destroy the local vars hash table every time.
 *
 * This can be fixed just implementing callframes caching
 * in Jim_CreateCallFrame() and Jim_FreeCallFrame(). */
int Jim_CallProcedure(Jim_Interp *interp, Jim_Cmd *cmd, int argc,
		Jim_Obj **argv)
{
	int i, retcode;
	Jim_CallFrame *callFramePtr;

	/* Check arity */
	if (argc < cmd->arityMin || (cmd->arityMax != -1 &&
	    argc > cmd->arityMax)) {
		Jim_Obj *objPtr = Jim_NewEmptyStringObj(interp);
		Jim_AppendStrings(interp, objPtr,
			"wrong # args: should be \"",
			Jim_GetString(argv[0], NULL),
			(cmd->arityMin > 1) ? " " : "",
			Jim_GetString(cmd->argListObjPtr, NULL),
			"\"", NULL);
		Jim_SetResult(interp, objPtr);
		return JIM_ERR;
	}
	/* Check if there are too nested calls */
	if (interp->numLevels == interp->maxNestingDepth) {
		Jim_SetResultString(interp,
			"Too many nested calls. Infinite recursion?", -1);
		return JIM_ERR;
	}
	/* Create a new callframe */
	callFramePtr = Jim_CreateCallFrame(interp);
	callFramePtr->parentCallFrame = interp->framePtr;
	callFramePtr->argv = argv;
	callFramePtr->argc = argc;
	callFramePtr->procArgsObjPtr = cmd->argListObjPtr;
	callFramePtr->procBodyObjPtr = cmd->bodyObjPtr;
	Jim_IncrRefCount(cmd->argListObjPtr);
	Jim_IncrRefCount(cmd->bodyObjPtr);
	interp->framePtr = callFramePtr;
	interp->numLevels ++;
	/* Set arguments */
	for (i = 0; i < cmd->arityMin-1; i++) {
		Jim_Obj *objPtr;

		Jim_ListIndex(interp, cmd->argListObjPtr, i, &objPtr, JIM_NONE);
		Jim_SetVariable(interp, objPtr, argv[i+1]);
	}
	if (cmd->arityMax == -1) {
		Jim_Obj *listObjPtr, *objPtr;

		listObjPtr = Jim_NewListObj(interp, argv+cmd->arityMin,
				argc-cmd->arityMin);
		Jim_ListIndex(interp, cmd->argListObjPtr, i, &objPtr, JIM_NONE);
		Jim_SetVariable(interp, objPtr, listObjPtr);
	}
	/* Eval the body */
	retcode = Jim_EvalObj(interp, cmd->bodyObjPtr);

	/* Destroy the callframe */
	interp->numLevels --;
	interp->framePtr = interp->framePtr->parentCallFrame;
	Jim_FreeCallFrame(interp, callFramePtr);

	/* Handle the return code */
	if (retcode == JIM_RETURN) {
		int returnCode = interp->returnCode;
		interp->returnCode = JIM_OK;
		return returnCode;
	}
	return retcode;
}

int Jim_Eval(Jim_Interp *interp, char *script)
{
	Jim_Obj *scriptObjPtr = Jim_NewStringObj(interp, script, -1);
	int retval;

	Jim_IncrRefCount(scriptObjPtr);
	retval = Jim_EvalObj(interp, scriptObjPtr);
	Jim_DecrRefCount(interp, scriptObjPtr);
	return retval;
}

int Jim_EvalFile(Jim_Interp *interp, char *filename)
{
	char *prg = NULL;
	FILE *fp;
	int nread, totread, maxlen, buflen;
	int retval;
	Jim_Obj *scriptObjPtr;
	
	if ((fp = fopen(filename, "r")) == NULL) {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
			"Error loading script \"", filename, "\": ",
			strerror(errno), NULL);
		return JIM_ERR;
	}
	buflen = 1024;
	maxlen = totread = 0;
	while (1) {
		if (maxlen < totread+buflen+1) {
			maxlen = totread+buflen+1;
			prg = Jim_Realloc(prg, maxlen);
		}
		if ((nread = fread(prg+totread, 1, buflen, fp)) == 0) break;
		totread += nread;
	}
	prg[totread] = '\0';
	fclose(fp);

	scriptObjPtr = Jim_NewStringObjNoAlloc(interp, prg, totread);
	Jim_SetSourceInfo(interp, scriptObjPtr, filename, 1);
	Jim_IncrRefCount(scriptObjPtr);
	retval = Jim_EvalObj(interp, scriptObjPtr);
	Jim_DecrRefCount(interp, scriptObjPtr);
	return retval;
}

/* -----------------------------------------------------------------------------
 * Subst
 * ---------------------------------------------------------------------------*/
static int JimParseSubstStr(struct JimParserCtx *pc)
{
	pc->tstart = pc->p;
	pc->tline = pc->linenr;
	while (*pc->p && *pc->p != '$' && *pc->p != '[')
		pc->p++;
	pc->tend = pc->p-1;
	pc->tt = JIM_TT_ESC;
	return JIM_OK;
}

static int JimParseSubst(struct JimParserCtx *pc, int flags)
{
	int retval;

	switch(*pc->p) {
	case '\0':
		pc->tstart = pc->tend = pc->p;
		pc->tline = pc->linenr;
		pc->tt = JIM_TT_EOL;
		pc->eof = 1;
		break;
	case '[':
		retval = JimParseCmd(pc);
		if (flags & JIM_SUBST_NOCMD) {
			pc->tstart--;
			pc->tend++;
			pc->tt = (flags & JIM_SUBST_NOESC) ?
				JIM_TT_STR : JIM_TT_ESC;
		}
		return retval;
		break;
	case '$':
		if (JimParseVar(pc) == JIM_ERR) {
			pc->tstart = pc->tend = pc->p++;
			pc->tline = pc->linenr;
			pc->tt = JIM_TT_STR;
		} else {
			if (flags & JIM_SUBST_NOVAR) {
				pc->tstart--;
				if (flags & JIM_SUBST_NOESC)
					pc->tt = JIM_TT_STR;
				else
					pc->tt = JIM_TT_ESC;
				if (*pc->tstart == '{') {
					pc->tstart--;
					if (*(pc->tend+1))
						pc->tend++;
				}
			}
		}
		break;
	default:
		retval = JimParseSubstStr(pc);
		if (flags & JIM_SUBST_NOESC)
			pc->tt = JIM_TT_STR;
		return retval;
		break;
	}
	return JIM_OK;
}

/* The subst object type reuses most of the data structures and functions
 * of the script object. Script's data structures are a bit more complex
 * for what is needed for [subst]itution tasks, but the reuse helps to
 * deal with a single data structure at the cost of some more memory
 * usage for substitutions. */
Jim_ObjType substObjType = {
	"subst",
	FreeScriptInternalRep,
	DupScriptInternalRep,
	NULL,
	JIM_TYPE_REFERENCES,
};

/* This method takes the string representation of an object
 * as a Tcl string where to perform [subst]itution, and generates
 * the pre-parsed internal representation. */
int SetSubstFromAny(Jim_Interp *interp, struct Jim_Obj *objPtr, int flags)
{
	char *scriptText = Jim_GetString(objPtr, NULL);
	struct JimParserCtx parser;
	struct ScriptObj *script = Jim_Alloc(sizeof(*script));

	script->len = 0;
	script->csLen = 0;
	script->commands = 0;
	script->token = NULL;
	script->cmdStruct = NULL;
	script->inUse = 1;
	script->substFlags = flags;
	script->fileName = NULL;

	JimParserInit(&parser, scriptText, 1);
	while(!JimParserEof(&parser)) {
		char *token;
		int len, type, linenr;

		JimParseSubst(&parser, flags);
		token = JimParserGetToken(&parser, &len, &type, &linenr);
		ScriptObjAddToken(interp, script, token, len, type,
				NULL, linenr);
	}
	/* Free the old internal rep and set the new one. */
	Jim_FreeIntRep(interp, objPtr);
	Jim_SetIntRepPtr(objPtr, script);
	objPtr->typePtr = &scriptObjType;
	return JIM_OK;
}

ScriptObj *Jim_GetSubst(Jim_Interp *interp, Jim_Obj *objPtr, int flags)
{
	struct ScriptObj *script = Jim_GetIntRepPtr(objPtr);

	if (objPtr->typePtr != &substObjType ||
	    script->substFlags != flags) {
		SetSubstFromAny(interp, objPtr, flags);
	}
	return (ScriptObj*) Jim_GetIntRepPtr(objPtr);
}

/* Performs commands,variables,blackslashes substitution,
 * storing the result object (with refcount 0) into
 * resObjPtrPtr. */
int Jim_SubstObj(Jim_Interp *interp, Jim_Obj *substObjPtr,
		Jim_Obj **resObjPtrPtr, int flags)
{
	ScriptObj *script;
	ScriptToken *token;
	int i, len, retcode = JIM_OK;
	Jim_Obj *resObjPtr, *savedResultObjPtr;

	Jim_IncrRefCount(substObjPtr); /* Make sure it's shared. */
	script = Jim_GetSubst(interp, substObjPtr, flags);
	/* In order to preserve the internal rep, we increment the
	 * inUse field of the script internal rep structure. */
	script->inUse++;

	token = script->token;
	len = script->len;

	/* Save the interp old result, to set it again before
	 * to return. */
	savedResultObjPtr = interp->result;
	Jim_IncrRefCount(savedResultObjPtr);
	
	/* Perform the substitution. Starts with an empty object
	 * and adds every token (performing the appropriate
	 * var/command/escape substitution). */
	resObjPtr = Jim_NewStringObj(interp, "", 0);
	for (i = 0; i < len; i++) {
		Jim_Obj *objPtr;

		switch(token[i].type) {
		case JIM_TT_STR:
		case JIM_TT_ESC:
			Jim_AppendObj(interp, resObjPtr, token[i].objPtr);
			break;
		case JIM_TT_VAR:
			objPtr = Jim_GetVariable(interp, token[i].objPtr,
					JIM_ERRMSG);
			if (objPtr == NULL) goto err;
			Jim_IncrRefCount(objPtr);
			Jim_AppendObj(interp, resObjPtr, objPtr);
			Jim_DecrRefCount(interp, objPtr);
			break;
		case JIM_TT_CMD:
			if (Jim_EvalObj(interp, token[i].objPtr) != JIM_OK)
				goto err;
			Jim_AppendObj(interp, resObjPtr, interp->result);
			break;
		case JIM_TT_EOL:
			break;
		default:
			Jim_Panic(
			  "default token type (%d) reached "
			  "in Jim_SubstObj().", token[i].type);
			break;
		}
	}
ok:
	if (retcode == JIM_OK)
		Jim_SetResult(interp, savedResultObjPtr);
	Jim_DecrRefCount(interp, savedResultObjPtr);
	/* Note that we don't have to decrement inUse, because the
	 * following code transfers our use of the reference again to
	 * the script object. */
	Jim_FreeIntRep(interp, substObjPtr);
	substObjPtr->typePtr = &scriptObjType;
	Jim_SetIntRepPtr(substObjPtr, script);
	Jim_DecrRefCount(interp, substObjPtr);
	*resObjPtrPtr = resObjPtr;
	return retcode;
err:
	Jim_IncrRefCount(resObjPtr);
	Jim_DecrRefCount(interp, resObjPtr);
	retcode = JIM_ERR;
	goto ok;
}

/* -----------------------------------------------------------------------------
 * API Input/Export functions
 * ---------------------------------------------------------------------------*/

void *Jim_GetApi(Jim_Interp *interp, char *funcname)
{
	Jim_HashEntry *he;

	he = Jim_FindHashEntry(&interp->stub, funcname);
	if (!he)
		return NULL;
	return he->val;
}

int Jim_RegisterApi(Jim_Interp *interp, char *funcname, void *funcptr)
{
	return Jim_AddHashEntry(&interp->stub, funcname, funcptr);
}

void Jim_RegisterCoreApi(Jim_Interp *interp)
{
  interp->getApiFuncPtr = Jim_GetApi;
  Jim_RegisterApi(interp, "Jim_EvalObj", Jim_EvalObj);
  Jim_RegisterApi(interp, "Jim_EvalObjVector", Jim_EvalObjVector);
  Jim_RegisterApi(interp, "Jim_InitHashTable", Jim_InitHashTable);
  Jim_RegisterApi(interp, "Jim_ExpandHashTable", Jim_ExpandHashTable);
  Jim_RegisterApi(interp, "Jim_AddHashEntry", Jim_AddHashEntry);
  Jim_RegisterApi(interp, "Jim_ReplaceHashEntry", Jim_ReplaceHashEntry);
  Jim_RegisterApi(interp, "Jim_DeleteHashEntry", Jim_DeleteHashEntry);
  Jim_RegisterApi(interp, "Jim_FreeHashTable", Jim_FreeHashTable);
  Jim_RegisterApi(interp, "Jim_FindHashEntry", Jim_FindHashEntry);
  Jim_RegisterApi(interp, "Jim_ResizeHashTable", Jim_ResizeHashTable);
  Jim_RegisterApi(interp, "Jim_GetHashTableIterator", Jim_GetHashTableIterator);
  Jim_RegisterApi(interp, "Jim_NextHashEntry", Jim_NextHashEntry);
  Jim_RegisterApi(interp, "Jim_NewObj", Jim_NewObj);
  Jim_RegisterApi(interp, "Jim_FreeObj", Jim_FreeObj);
  Jim_RegisterApi(interp, "Jim_InvalidateStringRep", Jim_InvalidateStringRep);
  Jim_RegisterApi(interp, "Jim_InitStringRep", Jim_InitStringRep);
  Jim_RegisterApi(interp, "Jim_DuplicateObj", Jim_DuplicateObj);
  Jim_RegisterApi(interp, "Jim_GetString", Jim_GetString);
  Jim_RegisterApi(interp, "Jim_InvalidateStringRep", Jim_InvalidateStringRep);
  Jim_RegisterApi(interp, "Jim_NewStringObj", Jim_NewStringObj);
  Jim_RegisterApi(interp, "Jim_NewStringObjNoAlloc", Jim_NewStringObjNoAlloc);
  Jim_RegisterApi(interp, "Jim_AppendString", Jim_AppendString);
  Jim_RegisterApi(interp, "Jim_AppendObj", Jim_AppendObj);
  Jim_RegisterApi(interp, "Jim_AppendStrings", Jim_AppendStrings);
  Jim_RegisterApi(interp, "Jim_StringEqObj", Jim_StringEqObj);
  Jim_RegisterApi(interp, "Jim_StringMatchObj", Jim_StringMatchObj);
  Jim_RegisterApi(interp, "Jim_StringRangeObj", Jim_StringRangeObj);
  Jim_RegisterApi(interp, "Jim_CompareStringImmediate", Jim_CompareStringImmediate);
  Jim_RegisterApi(interp, "Jim_NewReference", Jim_NewReference);
  Jim_RegisterApi(interp, "Jim_GetReference", Jim_GetReference);
  Jim_RegisterApi(interp, "Jim_CreateInterp", Jim_CreateInterp);
  Jim_RegisterApi(interp, "Jim_FreeInterp", Jim_FreeInterp);
  Jim_RegisterApi(interp, "Jim_RegisterCoreCommands", Jim_RegisterCoreCommands);
  Jim_RegisterApi(interp, "Jim_CreateCommand", Jim_CreateCommand);
  Jim_RegisterApi(interp, "Jim_CreateProcedure", Jim_CreateProcedure);
  Jim_RegisterApi(interp, "Jim_DeleteCommand", Jim_DeleteCommand);
  Jim_RegisterApi(interp, "Jim_RenameCommand", Jim_RenameCommand);
  Jim_RegisterApi(interp, "Jim_GetCommand", Jim_GetCommand);
  Jim_RegisterApi(interp, "Jim_SetVariable", Jim_SetVariable);
  Jim_RegisterApi(interp, "Jim_SetVariableLink", Jim_SetVariableLink);
  Jim_RegisterApi(interp, "Jim_GetVariable", Jim_GetVariable);
  Jim_RegisterApi(interp, "Jim_UnsetVariable", Jim_UnsetVariable);
  Jim_RegisterApi(interp, "Jim_GetCallFrameByLevel", Jim_GetCallFrameByLevel);
  Jim_RegisterApi(interp, "Jim_Collect", Jim_Collect);
  Jim_RegisterApi(interp, "Jim_CollectIfNeeded", Jim_CollectIfNeeded);
  Jim_RegisterApi(interp, "Jim_GetIndex", Jim_GetIndex);
  Jim_RegisterApi(interp, "Jim_NewListObj", Jim_NewListObj);
  Jim_RegisterApi(interp, "Jim_ListAppendElement", Jim_ListAppendElement);
  Jim_RegisterApi(interp, "Jim_ListAppendList", Jim_ListAppendList);
  Jim_RegisterApi(interp, "Jim_ListLength", Jim_ListLength);
  Jim_RegisterApi(interp, "Jim_ListIndex", Jim_ListIndex);
  Jim_RegisterApi(interp, "Jim_SetListIndex", Jim_SetListIndex);
  Jim_RegisterApi(interp, "Jim_ConcatObj", Jim_ConcatObj);
  Jim_RegisterApi(interp, "Jim_NewDictObj", Jim_NewDictObj);
  Jim_RegisterApi(interp, "Jim_DictKey", Jim_DictKey);
  Jim_RegisterApi(interp, "Jim_DictKeysVector", Jim_DictKeysVector);
  Jim_RegisterApi(interp, "Jim_GetIndex", Jim_GetIndex);
  Jim_RegisterApi(interp, "Jim_GetReturnCode", Jim_GetReturnCode);
  Jim_RegisterApi(interp, "Jim_EvalExpression", Jim_EvalExpression);
  Jim_RegisterApi(interp, "Jim_GetBoolFromExpr", Jim_GetBoolFromExpr);
  Jim_RegisterApi(interp, "Jim_GetWide", Jim_GetWide);
  Jim_RegisterApi(interp, "Jim_GetLong", Jim_GetLong);
  Jim_RegisterApi(interp, "Jim_SetWide", Jim_SetWide);
  Jim_RegisterApi(interp, "Jim_NewIntObj", Jim_NewIntObj);
  Jim_RegisterApi(interp, "Jim_WrongNumArgs", Jim_WrongNumArgs);
  Jim_RegisterApi(interp, "Jim_SetDictKeysVector", Jim_SetDictKeysVector);
  Jim_RegisterApi(interp, "Jim_SubstObj", Jim_SubstObj);
  Jim_RegisterApi(interp, "Jim_RegisterApi", Jim_RegisterApi);
}

/* -----------------------------------------------------------------------------
 * Core commands utility functions
 * ---------------------------------------------------------------------------*/
void Jim_WrongNumArgs(Jim_Interp *interp, int argc, Jim_Obj **argv, char *msg)
{
	int i;
	Jim_Obj *objPtr = Jim_NewEmptyStringObj(interp);

	Jim_AppendString(interp, objPtr, "wrong # args: should be \"", -1);
	for (i = 0; i < argc; i++) {
		Jim_AppendObj(interp, objPtr, argv[i]);
		Jim_AppendString(interp, objPtr, " ", 1);
	}
	Jim_AppendString(interp, objPtr, msg, -1);
	Jim_AppendString(interp, objPtr, "\"", 1);
	Jim_SetResult(interp, objPtr);
}

static Jim_Obj *Jim_CommandsList(Jim_Interp *interp, Jim_Obj *patternObjPtr)
{
	Jim_HashTableIterator *htiter;
	Jim_HashEntry *he;
	Jim_Obj *listObjPtr = Jim_NewListObj(interp, NULL, 0);
	char *pattern;
	
	pattern = patternObjPtr ? Jim_GetString(patternObjPtr, NULL) : NULL;
	htiter = Jim_GetHashTableIterator(&interp->commands);
	while ((he = Jim_NextHashEntry(htiter)) != NULL) {
		if (pattern && !Jim_StringMatch(pattern, he->key, 0))
			continue;
		Jim_ListAppendElement(interp, listObjPtr,
				Jim_NewStringObj(interp, he->key, -1));
	}
	Jim_FreeHashTableIterator(htiter);
	return listObjPtr;
}

static int Jim_InfoLevel(Jim_Interp *interp, Jim_Obj *levelObjPtr,
		Jim_Obj **objPtrPtr)
{
	Jim_CallFrame *targetCallFrame;

	if (Jim_GetCallFrameByLevel(interp, levelObjPtr, &targetCallFrame)
			!= JIM_OK)
		return JIM_ERR;
	/* No proc call at toplevel callframe */
	if (targetCallFrame == interp->topFramePtr) {
		Jim_SetResultString(interp, "Bad level", -1);
		return JIM_ERR;
	}
	*objPtrPtr = Jim_NewListObj(interp,
			targetCallFrame->argv,
			targetCallFrame->argc);
	return JIM_OK;
}

/* -----------------------------------------------------------------------------
 * Core commands
 * ---------------------------------------------------------------------------*/

/* fake [puts] -- not the real puts, just for debugging. */
int Jim_PutsCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	char *str;
	int len, nonewline = 0;
	
	if (argc != 2 && argc != 3) {
		Jim_WrongNumArgs(interp, 1, argv, "-nonewline string");
		return JIM_ERR;
	}
	if (argc == 3) {
		if (!Jim_CompareStringImmediate(interp, argv[1], "-nonewline"))
		{
			Jim_SetResultString(interp, "The second argument must "
					"be -nonewline", -1);
			return JIM_OK;
		} else {
			nonewline = 1;
			argv++;
		}
	}
	str = Jim_GetString(argv[1], &len);
	fwrite(str, 1, len, stdout);
	if (!nonewline) printf("\n");
	return JIM_OK;
}

/* Helper for [+] and [*] */
static int Jim_AddMulHelper(Jim_Interp *interp, int argc, Jim_Obj **argv,
		int op)
{
	jim_wide wideValue, res;
	double doubleValue, doubleRes;
	int i;

	res = (op == JIM_EXPROP_ADD) ? 0 : 1;
	
	for (i = 1; i < argc; i++) {
		if (Jim_GetWide(interp, argv[i], &wideValue) != JIM_OK)
			goto trydouble;
		if (op == JIM_EXPROP_ADD)
			res += wideValue;
		else
			res *= wideValue;
	}
	Jim_SetResult(interp, Jim_NewIntObj(interp, res));
	return JIM_OK;
trydouble:
	doubleRes = (double) res;
	for (;i < argc; i++) {
		if (Jim_GetDouble(interp, argv[i], &doubleValue) != JIM_OK)
			return JIM_ERR;
		if (op == JIM_EXPROP_ADD)
			doubleRes += doubleValue;
		else
			doubleRes *= doubleValue;
	}
	Jim_SetResult(interp, Jim_NewDoubleObj(interp, doubleRes));
	return JIM_OK;
}

/* Helper for [-] and [/] */
static int Jim_SubDivHelper(Jim_Interp *interp, int argc, Jim_Obj **argv,
		int op)
{
	jim_wide wideValue, res = 0;
	double doubleValue, doubleRes = 0;
	int i = 2;

	/* The arity = 2 case is different. For [- x] returns -x,
	 * while [/ x] returns 1/x. */
	if (argc == 2) {
		if (Jim_GetWide(interp, argv[1], &wideValue) != JIM_OK) {
			if (Jim_GetDouble(interp, argv[1], &doubleValue) !=
					JIM_OK)
			{
				return JIM_ERR;
			} else {
				if (op == JIM_EXPROP_SUB)
					doubleRes = -doubleValue;
				else
					doubleRes = 1.0/doubleValue;
				Jim_SetResult(interp, Jim_NewDoubleObj(interp,
							doubleRes));
				return JIM_OK;
			}
		}
		if (op == JIM_EXPROP_SUB) {
			res = -wideValue;
			Jim_SetResult(interp, Jim_NewIntObj(interp, res));
		} else {
			doubleRes = 1.0/wideValue;
			Jim_SetResult(interp, Jim_NewDoubleObj(interp,
						doubleRes));
		}
		return JIM_OK;
	} else {
		if (Jim_GetWide(interp, argv[1], &res) != JIM_OK) {
			if (Jim_GetDouble(interp, argv[1], &doubleRes)
					!= JIM_OK) {
				return JIM_ERR;
			} else {
				goto trydouble;
			}
		}
	}
	for (i = 2; i < argc; i++) {
		if (Jim_GetWide(interp, argv[i], &wideValue) != JIM_OK) {
			doubleRes = (double) res;
			goto trydouble;
		}
		if (op == JIM_EXPROP_SUB)
			res -= wideValue;
		else
			res /= wideValue;
	}
	Jim_SetResult(interp, Jim_NewIntObj(interp, res));
	return JIM_OK;
trydouble:
	for (;i < argc; i++) {
		if (Jim_GetDouble(interp, argv[i], &doubleValue) != JIM_OK)
			return JIM_ERR;
		if (op == JIM_EXPROP_SUB)
			doubleRes -= doubleValue;
		else
			doubleRes /= doubleValue;
	}
	Jim_SetResult(interp, Jim_NewDoubleObj(interp, doubleRes));
	return JIM_OK;
}


/* [+] */
int Jim_AddCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	return Jim_AddMulHelper(interp, argc, argv, JIM_EXPROP_ADD);
}

/* [*] */
int Jim_MulCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	return Jim_AddMulHelper(interp, argc, argv, JIM_EXPROP_MUL);
}

/* [-] */
int Jim_SubCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	return Jim_SubDivHelper(interp, argc, argv, JIM_EXPROP_SUB);
}

/* [/] */
int Jim_DivCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	return Jim_SubDivHelper(interp, argc, argv, JIM_EXPROP_DIV);
}

/* [set] */
int Jim_SetCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc != 2 && argc != 3) {
		Jim_WrongNumArgs(interp, 1, argv, "varName ?newValue?");
		return JIM_ERR;
	}
	if (argc == 2) {
		Jim_Obj *objPtr;
		objPtr = Jim_GetVariable(interp, argv[1], JIM_ERRMSG);
		if (!objPtr)
			return JIM_ERR;
		Jim_SetResult(interp, objPtr);
		return JIM_OK;
	}
	/* argc == 3 case. */
	if (Jim_SetVariable(interp, argv[1], argv[2]) != JIM_OK)
		return JIM_ERR;
	Jim_SetResult(interp, argv[2]);
	return JIM_OK;
}

/* [unset] */
int Jim_UnsetCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	int i;

	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "varName ?varName ...?");
		return JIM_ERR;
	}
	for (i = 1; i < argc; i++) {
		if (Jim_UnsetVariable(interp, argv[i], JIM_ERRMSG) != JIM_OK)
			return JIM_ERR;
	}
	return JIM_OK;
}

/* [incr] */
int Jim_IncrCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	jim_wide wideValue, increment = 1;
	Jim_Obj *intObjPtr;

	if (argc != 2 && argc != 3) {
		Jim_WrongNumArgs(interp, 1, argv, "varName ?increment?");
		return JIM_ERR;
	}
	if (argc == 3) {
		if (Jim_GetWide(interp, argv[2], &increment) != JIM_OK)
			return JIM_ERR;
	}
	intObjPtr = Jim_GetVariable(interp, argv[1], JIM_ERRMSG);
	if (!intObjPtr) return JIM_ERR;
	if (Jim_GetWide(interp, intObjPtr, &wideValue) != JIM_OK)
		return JIM_ERR;
	if (Jim_IsShared(intObjPtr)) {
		intObjPtr = Jim_NewIntObj(interp, wideValue+increment);
		if (Jim_SetVariable(interp, argv[1], intObjPtr) != JIM_OK) {
			Jim_IncrRefCount(intObjPtr);
			Jim_DecrRefCount(interp, intObjPtr);
			return JIM_ERR;
		}
	} else {
		Jim_SetWide(interp, intObjPtr, wideValue+increment);
	}
	Jim_SetResult(interp, intObjPtr);
	return JIM_OK;
}

/* [while] */
int Jim_WhileCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc != 3) {
		Jim_WrongNumArgs(interp, 1, argv, "condition body");
		return JIM_ERR;
	}
	while (1) {
		int boolean, retval;

		if ((retval = Jim_GetBoolFromExpr(interp, argv[1],
						&boolean)) != JIM_OK)
			return retval;
		if (!boolean) break;
		if ((retval = Jim_EvalObj(interp, argv[2])) != JIM_OK) {
			switch(retval) {
			case JIM_BREAK:
				goto out;
				break;
			case JIM_CONTINUE:
				continue;
				break;
			default:
				return retval;
			}
		}
	}
out:
	Jim_SetEmptyResult(interp);
	return JIM_OK;
}

/* [for] */
int Jim_ForCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	int retval;

	if (argc != 5) {
		Jim_WrongNumArgs(interp, 1, argv, "start test next body");
		return JIM_ERR;
	}
	/* Eval start */
	if ((retval = Jim_EvalObj(interp, argv[1])) != JIM_OK)
		return retval;
	while (1) {
		int boolean;
		/* Test the condition */
		if ((retval = Jim_GetBoolFromExpr(interp, argv[2], &boolean))
				!= JIM_OK)
			return retval;
		if (!boolean) break;
		/* Eval body */
		if ((retval = Jim_EvalObj(interp, argv[4])) != JIM_OK) {
			switch(retval) {
			case JIM_BREAK:
				goto out;
				break;
			case JIM_CONTINUE:
				continue;
				break;
			default:
				return retval;
			}
		}
		/* Eval next */
		if ((retval = Jim_EvalObj(interp, argv[3])) != JIM_OK) {
			switch(retval) {
			case JIM_BREAK:
				goto out;
				break;
			case JIM_CONTINUE:
				continue;
				break;
			default:
				return retval;
			}
		}
	}
out:
	Jim_SetEmptyResult(interp);
	return JIM_OK;
}

/* [if] */
int Jim_IfCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	int boolean, retval, current = 1, falsebody = 0;
	if (argc >= 3) {
		while (1) {
			/* Far not enough arguments given! */
			if (current >= argc) goto err;
			if ((retval = Jim_GetBoolFromExpr(interp,
						argv[current++], &boolean))
					!= JIM_OK)
				return retval;
			/* There lacks something, isn't it? */
			if (current >= argc) goto err;
			if (Jim_CompareStringImmediate(interp, argv[current],
						"then")) current++;
			/* Tsk tsk, no then-clause? */
			if (current >= argc) goto err;
			if (boolean)
				return Jim_EvalObj(interp, argv[current]);
			 /* Ok: no else-clause follows */
			if (++current >= argc) return JIM_OK;
			falsebody = current++;
			if (Jim_CompareStringImmediate(interp, argv[falsebody],
						"else")) {
				/* IIICKS - else-clause isn't last cmd? */
				if (current != argc-1) goto err;
				return Jim_EvalObj(interp, argv[current]);
			} else if (Jim_CompareStringImmediate(interp,
						argv[falsebody], "elseif"))
				/* Ok: elseif follows meaning all the stuff
				 * again (how boring...) */
				continue;
			/* OOPS - else-clause is not last cmd?*/
			else if (falsebody != argc-1)
				goto err;
			return Jim_EvalObj(interp, argv[falsebody]);
		}
		return JIM_OK;
	}
err:
	Jim_WrongNumArgs(interp, 1, argv, "condition ?then? trueBody ?elseif ...? ?else? falseBody");
	return JIM_ERR;
}

/* [list] */
int Jim_ListCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	Jim_Obj *listObjPtr;

	listObjPtr = Jim_NewListObj(interp, argv+1, argc-1);
	Jim_SetResult(interp, listObjPtr);
	return JIM_OK;
}

/* [lindex] */
int Jim_LindexCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	Jim_Obj *objPtr, *listObjPtr;
	int i;
	int index;

	if (argc < 3) {
		Jim_WrongNumArgs(interp, 1, argv, "listValue index ?...?");
		return JIM_ERR;
	}
	objPtr = argv[1];
	for (i = 2; i < argc; i++) {
		listObjPtr = objPtr;
		if (Jim_GetIndex(interp, argv[i], &index) != JIM_OK)
			return JIM_ERR;
		if (Jim_ListIndex(interp, listObjPtr, index, &objPtr,
					JIM_NONE) != JIM_OK) {
			/* Returns an empty object if the index
			 * is out of range. */
			Jim_SetEmptyResult(interp);
			return JIM_OK;
		}
	}
	Jim_SetResult(interp, objPtr);
	return JIM_OK;
}

/* [llenght] */
int Jim_LlengthCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	int len;

	if (argc != 2) {
		Jim_WrongNumArgs(interp, 1, argv, "listValue");
		return JIM_ERR;
	}
	Jim_ListLength(interp, argv[1], &len);
	Jim_SetResult(interp, Jim_NewIntObj(interp, len));
	return JIM_OK;
}

/* [lappend] */
int Jim_LappendCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	Jim_Obj *listObjPtr;
	int shared, i;

	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "listVar ?element ...?");
		return JIM_ERR;
	}
	listObjPtr = Jim_GetVariable(interp, argv[1], JIM_NONE);
	if (!listObjPtr) {
		/* Create the list if it does not exists */
		listObjPtr = Jim_NewListObj(interp, NULL, 0);
		if (Jim_SetVariable(interp, argv[1], listObjPtr) != JIM_OK) {
			Jim_IncrRefCount(listObjPtr);
			Jim_DecrRefCount(interp, listObjPtr);
			return JIM_ERR;
		}
	}
	shared = Jim_IsShared(listObjPtr);
	if (shared)
		listObjPtr = Jim_DuplicateObj(interp, listObjPtr);
	for (i = 2; i < argc; i++)
		Jim_ListAppendElement(interp, listObjPtr, argv[i]);
	if (shared) {
		if (Jim_SetVariable(interp, argv[1], listObjPtr) != JIM_OK) {
			Jim_IncrRefCount(listObjPtr);
			Jim_DecrRefCount(interp, listObjPtr);
			return JIM_ERR;
		}
	}
	Jim_SetResult(interp, listObjPtr);
	return JIM_OK;
}

/* [lset] */
int Jim_LsetCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc < 3) {
		Jim_WrongNumArgs(interp, 1, argv, "listVar ?index...? newVal");
		return JIM_ERR;
	} else if (argc == 3) {
		if (Jim_SetVariable(interp, argv[1], argv[2]) != JIM_OK)
			return JIM_ERR;
		Jim_SetResult(interp, argv[2]);
		return JIM_OK;
	}
	if (Jim_SetListIndex(interp, argv[1], argv+2, argc-3, argv[argc-1])
			== JIM_ERR) return JIM_ERR;
	return JIM_OK;
}

/* [append] */
int Jim_AppendCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	Jim_Obj *stringObjPtr;
	int shared, i;

	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "listVar ?string ...?");
		return JIM_ERR;
	}
	if (argc == 2) {
		stringObjPtr = Jim_GetVariable(interp, argv[1], JIM_ERRMSG);
		if (!stringObjPtr) return JIM_ERR;
	} else {
		stringObjPtr = Jim_GetVariable(interp, argv[1], JIM_NONE);
		if (!stringObjPtr) {
			/* Create the string if it does not exists */
			stringObjPtr = Jim_NewEmptyStringObj(interp);
			if (Jim_SetVariable(interp, argv[1], stringObjPtr)
					!= JIM_OK) {
				Jim_IncrRefCount(stringObjPtr);
				Jim_DecrRefCount(interp, stringObjPtr);
				return JIM_ERR;
			}
		}
	}
	shared = Jim_IsShared(stringObjPtr);
	if (shared)
		stringObjPtr = Jim_DuplicateObj(interp, stringObjPtr);
	for (i = 2; i < argc; i++)
		Jim_AppendObj(interp, stringObjPtr, argv[i]);
	if (shared) {
		if (Jim_SetVariable(interp, argv[1], stringObjPtr) != JIM_OK) {
			Jim_IncrRefCount(stringObjPtr);
			Jim_DecrRefCount(interp, stringObjPtr);
			return JIM_ERR;
		}
	}
	Jim_SetResult(interp, stringObjPtr);
	return JIM_OK;
}

/* [debug] */
int Jim_DebugCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	char *subcommand;

	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "option ?...?");
		return JIM_ERR;
	}
	subcommand = Jim_GetString(argv[1], NULL);
	if (!strcmp(subcommand, "refcount")) {
		if (argc != 3) {
			Jim_WrongNumArgs(interp, 2, argv, "object");
			return JIM_ERR;
		}
		Jim_SetResult(interp, Jim_NewIntObj(interp, argv[2]->refCount));
		return JIM_OK;
	} else if (!strcmp(subcommand, "objcount")) {
		int freeobj = 0, liveobj = 0;
		char buf[256];
		Jim_Obj *objPtr;

		if (argc != 2) {
			Jim_WrongNumArgs(interp, 2, argv, "");
			return JIM_ERR;
		}
		/* Count the number of free objects. */
		objPtr = interp->freeList;
		while (objPtr) {
			freeobj++;
			objPtr = objPtr->nextObjPtr;
		}
		/* Count the number of live objects. */
		objPtr = interp->liveList;
		while (objPtr) {
			liveobj++;
			objPtr = objPtr->nextObjPtr;
		}
		/* Set the result string and return. */
		sprintf(buf, "free %d used %d", freeobj, liveobj);
		Jim_SetResultString(interp, buf, -1);
		return JIM_OK;
	} else if (!strcmp(subcommand, "objects")) {
		Jim_Obj *objPtr, *listObjPtr, *subListObjPtr;
		/* Count the number of live objects. */
		objPtr = interp->liveList;
		listObjPtr = Jim_NewListObj(interp, NULL, 0);
		while (objPtr) {
			char buf[128];
			char *type = objPtr->typePtr ?
				objPtr->typePtr->name : "";
			subListObjPtr = Jim_NewListObj(interp, NULL, 0);
			sprintf(buf, "%p", objPtr);
			Jim_ListAppendElement(interp, subListObjPtr,
				Jim_NewStringObj(interp, buf, -1));
			Jim_ListAppendElement(interp, subListObjPtr,
				Jim_NewStringObj(interp, type, -1));
			Jim_ListAppendElement(interp, subListObjPtr,
				Jim_NewIntObj(interp, objPtr->refCount));
			Jim_ListAppendElement(interp, subListObjPtr, objPtr);
			Jim_ListAppendElement(interp, listObjPtr, subListObjPtr);
			objPtr = objPtr->nextObjPtr;
		}
		Jim_SetResult(interp, listObjPtr);
		return JIM_OK;
	} else if (!strcmp(subcommand, "invstr")) {
		Jim_Obj *objPtr;

		if (argc != 3) {
			Jim_WrongNumArgs(interp, 2, argv, "object");
			return JIM_ERR;
		}
		objPtr = argv[2];
		if (objPtr->typePtr != NULL)
			Jim_InvalidateStringRep(objPtr);
		Jim_SetEmptyResult(interp);
		return JIM_OK;
	} else {
		Jim_SetResultString(interp,
			"bad option. Valid options are refcount, "
			"objcount, objects, invstr", -1);
		return JIM_ERR;
	}
	return JIM_OK; /* unreached */
}

/* [eval] */
int Jim_EvalCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc == 2) {
		return Jim_EvalObj(interp, argv[1]);
	} else if (argc > 2) {
		Jim_Obj *objPtr;
		int retcode;

		objPtr = Jim_ConcatObj(interp, argc-1, argv+1);
		Jim_IncrRefCount(objPtr);
		retcode = Jim_EvalObj(interp, objPtr);
		Jim_DecrRefCount(interp, objPtr);
		return retcode;
	} else {
		Jim_WrongNumArgs(interp, 1, argv, "script ?...?");
		return JIM_ERR;
	}
}

/* [uplevel] */
int Jim_UplevelCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc >= 2) {
		int retcode;
		Jim_CallFrame *savedCallFrame, *targetCallFrame;
		Jim_Obj *objPtr;
		char *str;

		/* Save the old callframe pointer */
		savedCallFrame = interp->framePtr;

		/* Lookup the target frame pointer */
		str = Jim_GetString(argv[1], NULL);
		if (argc >= 3 && 
		    ((str[0] >= '0' && str[0] <= '9') || str[0] == '#'))
		{
			if (Jim_GetCallFrameByLevel(interp, argv[1],
						&targetCallFrame) != JIM_OK)
				return JIM_ERR;
			argc--;
			argv++;
		} else {
			if (Jim_GetCallFrameByLevel(interp, NULL,
						&targetCallFrame) != JIM_OK)
				return JIM_ERR;
		}
		/* Eval the code in the target callframe. */
		interp->framePtr = targetCallFrame;
		if (argc == 2) {
			retcode = Jim_EvalObj(interp, argv[1]);
		} else {
			objPtr = Jim_ConcatObj(interp, argc-1, argv+1);
			Jim_IncrRefCount(objPtr);
			retcode = Jim_EvalObj(interp, objPtr);
			Jim_DecrRefCount(interp, objPtr);
		}
		interp->framePtr = savedCallFrame;
		return retcode;
	} else {
		Jim_WrongNumArgs(interp, 1, argv, "?level? script ?...?");
		return JIM_ERR;
	}
}

/* [expr] */
int Jim_ExprCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	Jim_Obj *exprResultPtr;
	int retcode;

	if (argc == 2) {
		retcode = Jim_EvalExpression(interp, argv[1], &exprResultPtr);
	} else if (argc > 2) {
		Jim_Obj *objPtr;

		objPtr = Jim_ConcatObj(interp, argc-1, argv+1);
		Jim_IncrRefCount(objPtr);
		retcode = Jim_EvalExpression(interp, objPtr, &exprResultPtr);
		Jim_DecrRefCount(interp, objPtr);
	} else {
		Jim_WrongNumArgs(interp, 1, argv, "expression ?...?");
		return JIM_ERR;
	}
	if (retcode != JIM_OK) return retcode;
	Jim_SetResult(interp, exprResultPtr);
	Jim_DecrRefCount(interp, exprResultPtr);
	return JIM_OK;
}

/* [break] */
int Jim_BreakCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc != 1) {
		Jim_WrongNumArgs(interp, 1, argv, "");
		return JIM_ERR;
	}
	return JIM_BREAK;
}

/* [continue] */
int Jim_ContinueCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc != 1) {
		Jim_WrongNumArgs(interp, 1, argv, "");
		return JIM_ERR;
	}
	return JIM_CONTINUE;
}

/* [return] */
int Jim_ReturnCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc == 1) {
		return JIM_RETURN;
	} else if (argc == 2) {
		Jim_SetResult(interp, argv[1]);
		interp->returnCode = JIM_OK;
		return JIM_RETURN;
	} else if (argc == 3 || argc == 4) {
		int returnCode;
		if (Jim_GetReturnCode(interp, argv[2], &returnCode) == JIM_ERR)
			return JIM_ERR;
		interp->returnCode = returnCode;
		if (argc == 4)
			Jim_SetResult(interp, argv[3]);
		return JIM_RETURN;
	} else {
		Jim_WrongNumArgs(interp, 1, argv, "?-code code? ?result?");
		return JIM_ERR;
	}
	return JIM_CONTINUE;
}

/* [proc] */
int Jim_ProcCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	int argListLen;
	int arityMin, arityMax;

	if (argc != 4) {
		Jim_WrongNumArgs(interp, 1, argv, "name arglist body");
		return JIM_ERR;
	}
	Jim_ListLength(interp, argv[2], &argListLen);
	arityMin = arityMax = argListLen+1;
	if (argListLen) {
		char *str;
		int len;
		Jim_Obj *lastArgPtr;
		
		Jim_ListIndex(interp, argv[2], argListLen-1, &lastArgPtr, JIM_NONE);
		str = Jim_GetString(lastArgPtr, &len);
		if (len == 4 && memcmp(str, "args", 4) == 0) {
			arityMin--;
			arityMax = -1;
		}
	}
	Jim_CreateProcedure(interp, Jim_GetString(argv[1], NULL),
			argv[2], argv[3], arityMin, arityMax);
	return JIM_OK;
}

/* [concat] */
int Jim_ConcatCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	Jim_SetResult(interp, Jim_ConcatObj(interp, argc-1, argv+1));
	return JIM_OK;
}

/* [upvar] */
int Jim_UpvarCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	char *str;
	int i;
	Jim_CallFrame *targetCallFrame;

	/* Lookup the target frame pointer */
	str = Jim_GetString(argv[1], NULL);
	if (argc > 3 && 
	    ((str[0] >= '0' && str[0] <= '9') || str[0] == '#'))
	{
		if (Jim_GetCallFrameByLevel(interp, argv[1],
					&targetCallFrame) != JIM_OK)
			return JIM_ERR;
		argc--;
		argv++;
	} else {
		if (Jim_GetCallFrameByLevel(interp, NULL,
					&targetCallFrame) != JIM_OK)
			return JIM_ERR;
	}
	/* Check for arity */
	if (argc < 3 || ((argc-1)%2) != 0) {
		Jim_WrongNumArgs(interp, 1, argv, "?level? otherVar localVar ?otherVar localVar ...?");
		return JIM_ERR;
	}
	/* Now... for every other/local couple: */
	for (i = 1; i < argc; i += 2) {
		if (Jim_SetVariableLink(interp, argv[i+1], argv[i],
				targetCallFrame) != JIM_OK) return JIM_ERR;
	}
	return JIM_OK;
}

/* [global] */
int Jim_GlobalCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	int i;

	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "varName ?varName ...?");
		return JIM_ERR;
	}
	/* Link every var to the toplevel having the same name */
	if (interp->numLevels == 0) return JIM_OK; /* global at toplevel... */
	for (i = 1; i < argc; i++) {
		if (Jim_SetVariableLink(interp, argv[i], argv[i],
				interp->topFramePtr) != JIM_OK) return JIM_ERR;
	}
	return JIM_OK;
}

/* [string] */
int Jim_StringCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "option ?arguments ...?");
		return JIM_ERR;
	}
	if (Jim_CompareStringImmediate(interp, argv[1], "length")) {
		int len;

		if (argc != 3) {
			Jim_WrongNumArgs(interp, 2, argv, "string");
			return JIM_ERR;
		}
		Jim_GetString(argv[2], &len);
		Jim_SetResult(interp, Jim_NewIntObj(interp, len));
		return JIM_OK;
	} else if (Jim_CompareStringImmediate(interp, argv[1], "compare")) {
		char *a, *b;

		if (argc != 4) {
			Jim_WrongNumArgs(interp, 2, argv, "string1 string2");
			return JIM_ERR;
		}
		a = Jim_GetString(argv[2], NULL);
		b = Jim_GetString(argv[3], NULL);
		Jim_SetResult(interp, Jim_NewIntObj(interp, strcmp(a,b)));
		return JIM_OK;
	} else if (Jim_CompareStringImmediate(interp, argv[1], "match")) {
		int nocase = 0;
		if ((argc != 4 && argc != 5) ||
		    (argc == 5 && Jim_CompareStringImmediate(interp,
				argv[2], "-nocase") == 0)) {
			Jim_WrongNumArgs(interp, 2, argv, "?-nocase? pattern "
					"string");
			return JIM_ERR;
		}
		if (argc == 5) {
			nocase = 1;
			argv++;
		}
		Jim_SetResult(interp,
			Jim_NewIntObj(interp, Jim_StringMatchObj(argv[2],
					argv[3], nocase)));
		return JIM_OK;
	} else if (Jim_CompareStringImmediate(interp, argv[1], "equal")) {
		if (argc != 4) {
			Jim_WrongNumArgs(interp, 2, argv, "string1 string2");
			return JIM_ERR;
		}
		Jim_SetResult(interp,
			Jim_NewIntObj(interp, Jim_StringEqObj(argv[2],
					argv[3], 0)));
		return JIM_OK;
	} else if (Jim_CompareStringImmediate(interp, argv[1], "range")) {
		Jim_Obj *objPtr;

		if (argc != 5) {
			Jim_WrongNumArgs(interp, 2, argv, "string first last");
			return JIM_ERR;
		}
		objPtr = Jim_StringRangeObj(interp, argv[2], argv[3], argv[4]);
		if (objPtr == NULL)
			return JIM_ERR;
		Jim_SetResult(interp, objPtr);
		return JIM_OK;
	} else {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
			"bad option \"", Jim_GetString(argv[1], NULL), "\":",
			" must be length, compare, match, equal, range",
			NULL);
		return JIM_ERR;
	}
	return JIM_OK;
}

/* [time] */
int Jim_TimeCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	long i, count = 1, start, elapsed;
	char buf [256];

	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "script ?count?");
		return JIM_ERR;
	}
	if (argc == 3) {
		if (Jim_GetLong(interp, argv[2], &count) != JIM_OK)
			return JIM_ERR;
	}
	if (count < 0)
		return JIM_OK;
	i = count;
	start = Jim_Clock();
	while (i-- > 0) {
		int retval;

		if ((retval = Jim_EvalObj(interp, argv[1])) != JIM_OK)
			return retval;
	}
	elapsed = Jim_Clock() - start;
	sprintf(buf, "%ld microseconds per iteration", elapsed/count);
	Jim_SetResultString(interp, buf, -1);
	return JIM_OK;
}

/* [exit] */
int Jim_ExitCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	long exitCode = 0;

	if (argc > 2) {
		Jim_WrongNumArgs(interp, 1, argv, "?exitCode?");
		return JIM_ERR;
	}
	if (argc == 2) {
		if (Jim_GetLong(interp, argv[1], &exitCode) != JIM_OK)
			return JIM_ERR;
	}
	exit(exitCode);
	return JIM_OK; /* unreached */
}

/* [catch] */
int Jim_CatchCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	int exitCode = 0;

	if (argc != 2 && argc != 3) {
		Jim_WrongNumArgs(interp, 1, argv, "script ?varName?");
		return JIM_ERR;
	}
	exitCode = Jim_EvalObj(interp, argv[1]);
	if (argc == 3) {
		if (Jim_SetVariable(interp, argv[2], Jim_GetResult(interp))
				!= JIM_OK)
			return JIM_ERR;
	}
	Jim_SetResult(interp, Jim_NewIntObj(interp, exitCode));
	return JIM_OK;
}

/* [ref] */
int Jim_RefCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc != 2 && argc != 3) {
		Jim_WrongNumArgs(interp, 1, argv, "string ?finalizer?");
		return JIM_ERR;
	}
	if (argc == 2) {
		Jim_SetResult(interp, Jim_NewReference(interp, argv[1], NULL));
	} else {
		Jim_SetResult(interp, Jim_NewReference(interp, argv[1], argv[2]));
	}
	return JIM_OK;
}

/* [getref] */
int Jim_GetrefCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	Jim_Reference *refPtr;

	if (argc != 2) {
		Jim_WrongNumArgs(interp, 1, argv, "reference");
		return JIM_ERR;
	}
	if ((refPtr = Jim_GetReference(interp, argv[1])) == NULL)
		return JIM_ERR;
	Jim_SetResult(interp, refPtr->objPtr);
	return JIM_OK;
}

/* [setref] */
int Jim_SetrefCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	Jim_Reference *refPtr;

	if (argc != 3) {
		Jim_WrongNumArgs(interp, 1, argv, "reference newValue");
		return JIM_ERR;
	}
	if ((refPtr = Jim_GetReference(interp, argv[1])) == NULL)
		return JIM_ERR;
	Jim_IncrRefCount(argv[2]);
	Jim_DecrRefCount(interp, refPtr->objPtr);
	refPtr->objPtr = argv[2];
	Jim_SetResult(interp, argv[1]);
	return JIM_OK;
}

/* [collect] */
int Jim_CollectCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc != 1) {
		Jim_WrongNumArgs(interp, 1, argv, "");
		return JIM_ERR;
	}
	Jim_SetResult(interp, Jim_NewIntObj(interp, Jim_Collect(interp)));
	return JIM_OK;
}

/* TODO */
/* [finalize] reference ?newValue? */
/* [references] (list of all the references/finalizers) */

/* [rename] */
int Jim_RenameCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	char *oldName, *newName;

	if (argc != 3) {
		Jim_WrongNumArgs(interp, 1, argv, "oldName newName");
		return JIM_ERR;
	}
	oldName = Jim_GetString(argv[1], NULL);
	newName = Jim_GetString(argv[2], NULL);
	if (Jim_RenameCommand(interp, oldName, newName) != JIM_OK) {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
			"can't rename \"", oldName, "\": ",
			"command doesn't exist", NULL);
		return JIM_ERR;
	}
	return JIM_OK;
}

/* [dict] */
int Jim_DictCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "option ?arguments ...?");
		return JIM_ERR;
	}
	if (Jim_CompareStringImmediate(interp, argv[1], "create")) {
		Jim_Obj *objPtr;

		if (argc % 2) {
			Jim_WrongNumArgs(interp, 2, argv, "?key value ...?");
			return JIM_ERR;
		}
		objPtr = Jim_NewDictObj(interp, argv+2, argc-2);
		Jim_SetResult(interp, objPtr);
		return JIM_OK;
	} else if (Jim_CompareStringImmediate(interp, argv[1], "get")) {
		Jim_Obj *objPtr;

		if (Jim_DictKeysVector(interp, argv[2], argv+3, argc-3, &objPtr,
				JIM_ERRMSG) != JIM_OK)
			return JIM_ERR;
		Jim_SetResult(interp, objPtr);
		return JIM_OK;
	} else if (Jim_CompareStringImmediate(interp, argv[1], "set")) {
		if (argc < 5) {
			Jim_WrongNumArgs(interp, 2, argv, "varName key ?key ...? value");
			return JIM_ERR;
		}
		return Jim_SetDictKeysVector(interp, argv[2], argv+3, argc-4,
					argv[argc-1]);
	} else if (Jim_CompareStringImmediate(interp, argv[1], "unset")) {
		if (argc < 4) {
			Jim_WrongNumArgs(interp, 2, argv, "varName key ?key ...?");
			return JIM_ERR;
		}
		return Jim_SetDictKeysVector(interp, argv[2], argv+3, argc-3,
					NULL);
	} else if (Jim_CompareStringImmediate(interp, argv[1], "exists")) {
		Jim_Obj *objPtr;
		int exists;

		if (Jim_DictKeysVector(interp, argv[2], argv+3, argc-3, &objPtr,
				JIM_ERRMSG) == JIM_OK)
			exists = 1;
		else
			exists = 0;
		Jim_SetResult(interp, Jim_NewIntObj(interp, exists));
		return JIM_OK;
	} else {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
			"bad option \"", Jim_GetString(argv[1], NULL), "\":",
			" must be create, get, set", NULL);
		return JIM_ERR;
	}
	return JIM_OK;
}

/* [load] */
int Jim_LoadCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "libaryFile");
		return JIM_ERR;
	}
	return Jim_LoadLibrary(interp, Jim_GetString(argv[1], NULL));
}

/* [subst] */
int Jim_SubstCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	int i, flags = 0;
	Jim_Obj *objPtr;

	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv,
			"?-nobackslashes? ?-nocommands? ?-novariables? string");
		return JIM_ERR;
	}
	i = argc-2;
	while(i--) {
		if (Jim_CompareStringImmediate(interp, argv[i+1],
					"-nobackslashes"))
			flags |= JIM_SUBST_NOESC;
		else if (Jim_CompareStringImmediate(interp, argv[i+1],
					"-novariables"))
			flags |= JIM_SUBST_NOVAR;
		else if (Jim_CompareStringImmediate(interp, argv[i+1],
					"-nocommands"))
			flags |= JIM_SUBST_NOCMD;
		else {
			Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
			Jim_AppendStrings(interp, Jim_GetResult(interp),
				"bad option \"", Jim_GetString(argv[i+1], NULL),
				"\": must be -nobackslashes, -nocommands, or "
				"-novariables", NULL);
			return JIM_ERR;
		}
	}
	if (Jim_SubstObj(interp, argv[argc-1], &objPtr, flags) != JIM_OK)
		return JIM_ERR;
	Jim_SetResult(interp, objPtr);
	return JIM_OK;
}

/* [info] */
int Jim_InfoCoreCommand(Jim_Interp *interp, int argc, Jim_Obj **argv)
{
	if (argc < 2) {
		Jim_WrongNumArgs(interp, 1, argv, "option ?args ...?");
		return JIM_ERR;
	}
	if (Jim_CompareStringImmediate(interp, argv[1], "commands")) {
		if (argc != 2 && argc != 3) {
			Jim_WrongNumArgs(interp, 2, argv, "?pattern?");
			return JIM_ERR;
		}
		if (argc == 3)
			Jim_SetResult(interp,Jim_CommandsList(interp, argv[2]));
		else
			Jim_SetResult(interp, Jim_CommandsList(interp, NULL));
		return JIM_OK;
	} else if (Jim_CompareStringImmediate(interp, argv[1], "level")) {
		Jim_Obj *objPtr;

		if (argc != 2 && argc != 3) {
			Jim_WrongNumArgs(interp, 2, argv, "?levelNum?");
			return JIM_ERR;
		}
		if (argc == 2) {
			Jim_SetResult(interp,
				Jim_NewIntObj(interp, interp->numLevels));
			return JIM_OK;
		}
		/* argc == 3 case: */
		if (Jim_InfoLevel(interp, argv[2], &objPtr) != JIM_OK)
			return JIM_ERR;
		Jim_SetResult(interp, objPtr);
		return JIM_OK;
	} else {
		Jim_SetResult(interp, Jim_NewEmptyStringObj(interp));
		Jim_AppendStrings(interp, Jim_GetResult(interp),
			"bad option \"", Jim_GetString(argv[1], NULL), "\":",
			" must be commands, level", NULL);
		return JIM_ERR;
	}
}

struct {
	char *name;
	Jim_CmdProc cmdProc;
	int arityMin, arityMax;
} Jim_CoreCommandsTable[] = {
	{"set", Jim_SetCoreCommand, 2, 3},
	{"unset", Jim_UnsetCoreCommand, 2, -1},
	{"puts", Jim_PutsCoreCommand, 2, 2},
	{"+", Jim_AddCoreCommand, 1, -1},
	{"*", Jim_MulCoreCommand, 1, -1},
	{"-", Jim_SubCoreCommand, 2, -1},
	{"/", Jim_DivCoreCommand, 2, -1},
	{"incr", Jim_IncrCoreCommand, 2, 3},
	{"while", Jim_WhileCoreCommand, 3, 3},
	{"for", Jim_ForCoreCommand, 5, 5},
	{"if", Jim_IfCoreCommand, 3, -1},
	{"list", Jim_ListCoreCommand, 1, -1},
	{"lindex", Jim_LindexCoreCommand, 3, -1},
	{"lset", Jim_LsetCoreCommand, 4, -1},
	{"llength", Jim_LlengthCoreCommand, 2, 2},
	{"lappend", Jim_LappendCoreCommand, 2, -1},
	{"append", Jim_AppendCoreCommand, 2, -1},
	{"debug", Jim_DebugCoreCommand, 2, -1},
	{"eval", Jim_EvalCoreCommand, 2, -1},
	{"uplevel", Jim_UplevelCoreCommand, 2, -1},
	{"expr", Jim_ExprCoreCommand, 2, -1},
	{"break", Jim_BreakCoreCommand, 1, 1},
	{"continue", Jim_ContinueCoreCommand, 1, 1},
	{"proc", Jim_ProcCoreCommand, 4, 4},
	{"concat", Jim_ConcatCoreCommand, 1, -1},
	{"return", Jim_ReturnCoreCommand, 1, 4},
	{"upvar", Jim_UpvarCoreCommand, 3, -1},
	{"global", Jim_GlobalCoreCommand, 2, -1},
	{"string", Jim_StringCoreCommand, 3, -1},
	{"time", Jim_TimeCoreCommand, 2, 3},
	{"exit", Jim_ExitCoreCommand, 1, 2},
	{"catch", Jim_CatchCoreCommand, 2, 3},
	{"ref", Jim_RefCoreCommand, 2, 3},
	{"getref", Jim_GetrefCoreCommand, 2, 2},
	{"setref", Jim_SetrefCoreCommand, 3, 3},
	{"collect", Jim_CollectCoreCommand, 1, 1},
	{"rename", Jim_RenameCoreCommand, 3, 3},
	{"dict", Jim_DictCoreCommand, 2, -1},
	{"load", Jim_LoadCoreCommand, 2, 2},
	{"subst", Jim_SubstCoreCommand, 2, -1},
	{"info", Jim_InfoCoreCommand, 2, -1},
	{NULL, NULL, 0, 0}
};

/* Some Jim core command is actually a procedure written in Jim itself. */
static void Jim_RegisterCoreProcedures(Jim_Interp *interp)
{
	Jim_Eval(interp,
"proc lambda {arglist body} {\n"
"    set name [ref {} lambdaFinalizer]\n"
"    proc $name $arglist $body\n"
"    return $name\n"
"}\n"
"proc lambdaFinalizer {name val} {\n"
"    rename $name {}\n"
"}\n"
	);
}

void Jim_RegisterCoreCommands(Jim_Interp *interp)
{
	int i = 0;

	while(Jim_CoreCommandsTable[i].name != NULL) {
		Jim_CreateCommand(interp, 
				Jim_CoreCommandsTable[i].name,
				Jim_CoreCommandsTable[i].cmdProc,
				Jim_CoreCommandsTable[i].arityMin,
				Jim_CoreCommandsTable[i].arityMax,
				NULL);
		i++;
	}
	Jim_RegisterCoreProcedures(interp);
}

/* -----------------------------------------------------------------------------
 * Test
 * ---------------------------------------------------------------------------*/
int test_parser(char *filename, int parsetype)
{
	struct JimParserCtx parser;
	char prg[1024];
	FILE *fp;
	int nread;

	if ((fp = fopen(filename, "r")) == NULL) {
		perror("fopen");
		exit(1);
	}
	nread = fread(prg, 1, 1024, fp);
	prg[nread] = '\0';
	fclose(fp);

	JimParserInit(&parser, prg, 1);
	while(!JimParserEof(&parser)) {
		char *type = "", *tok;
		int len, retval = 0;
		if (parsetype == 0)
			retval = JimParseScript(&parser);
		else if (parsetype == 1)
			retval = JimParseExpression(&parser);
		else if (parsetype == 2)
			retval = JimParseSubst(&parser, 0);
		if (retval != JIM_OK) {
			printf("PARSE ERROR\n");
			exit(1);
		}
		switch(JimParserTtype(&parser)) {
		case JIM_TT_STR: type = "STR"; break;
		case JIM_TT_ESC: type = "ESC"; break;
		case JIM_TT_VAR: type = "VAR"; break;
		case JIM_TT_DICTSUGAR: type = "DICTSUGAR"; break;
		case JIM_TT_CMD: type = "CMD"; break;
		case JIM_TT_SEP: type = "SEP"; break;
		case JIM_TT_EOL: type = "EOL"; break;
		case JIM_TT_NONE: type = "NONE"; break;
		case JIM_TT_SUBEXPR_START: type = "SUBEXPR_START"; break;
		case JIM_TT_SUBEXPR_END : type = "SUBEXPR_END"; break;
		case JIM_TT_EXPR_NUMBER : type = "EXPR_NUMBER"; break;
		case JIM_TT_EXPR_OPERATOR: type = "EXPR_OPERATOR"; break;
		}
		printf("%d %s: ", JimParserTline(&parser), type);
		tok = JimParserGetToken(&parser, &len, NULL, NULL);
		printf("'%s' (%d)\n", tok, len);
	}
	return 0;
}

void Jim_PrintErrorMessage(Jim_Interp *interp)
{
	int len, i;

	printf("Runtime error, file \"%s\", line %d:\n",
			interp->errorFileName, interp->errorLine);
	printf("    %s\n", Jim_GetString(interp->result, NULL));
	Jim_ListLength(interp, interp->stackTrace, &len);
	for (i = 0; i < len; i+= 3) {
		Jim_Obj *objPtr;
		char *proc, *file, *line;

		Jim_ListIndex(interp, interp->stackTrace, i, &objPtr, JIM_NONE);
		proc = Jim_GetString(objPtr, NULL);
		Jim_ListIndex(interp, interp->stackTrace, i+1, &objPtr,
				JIM_NONE);
		file = Jim_GetString(objPtr, NULL);
		Jim_ListIndex(interp, interp->stackTrace, i+2, &objPtr,
				JIM_NONE);
		line = Jim_GetString(objPtr, NULL);
		printf("In procedure '%s' called at file \"%s\", line %s\n",
				proc, file, line);
	}
}

int Jim_InteractivePrompt(void)
{
	Jim_Interp *interp = Jim_CreateInterp();
	int retcode = JIM_OK;

	Jim_RegisterCoreCommands(interp);
	printf("Welcome to Jim version %d, "
	       "Copyright (c) 2005 Salvatore Sanfilippo\n", JIM_VERSION);
	while (1) {
		char prg[1024], *result;
		int reslen;

		printf("%d jim> ", retcode);
		fflush(stdout);
		if (fgets(prg, 1024, stdin) == NULL) break;
		retcode = Jim_Eval(interp, prg);
		result = Jim_GetString(Jim_GetResult(interp), &reslen);
		if (retcode == JIM_ERR) {
			Jim_PrintErrorMessage(interp);
		} else {
			if (reslen) {
				printf("%s\n", result);
			}
		}
	}
	Jim_FreeInterp(interp);
	return 0;
}

int main(int argc, char **argv)
{
	int retcode;
	Jim_Interp *interp;
	
	if (argc == 1)
		return Jim_InteractivePrompt();

	/* Parser? */
	if (argc == 3 && !strcmp(argv[1], "--parse")) {
		test_parser(argv[2], 0);
		return 0;
	} else if (argc == 3 && !strcmp(argv[1], "--parse-expr")) {
		test_parser(argv[2], 1);
		return 0;
	} else if (argc == 3 && !strcmp(argv[1], "--parse-subst")) {
		test_parser(argv[2], 2);
		return 0;
	} else if (argc == 2 && !strcmp(argv[1], "--test-ht")) {
		testHashTable();
		return 0;
	}

	/* Load the program */
	if (argc != 2) {
		fprintf(stderr, "missing filename\n");
		exit(1);
	}

	/* Run it */
	interp = Jim_CreateInterp();
	Jim_RegisterCoreCommands(interp);
	if ((retcode = Jim_EvalFile(interp, argv[1])) == JIM_ERR) {
		Jim_PrintErrorMessage(interp);
	}
	Jim_FreeInterp(interp);
	return retcode;
}