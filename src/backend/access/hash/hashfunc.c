/*-------------------------------------------------------------------------
 *
 * hashfunc.c
 *      Support functions for hash access method.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *      src/backend/access/hash/hashfunc.c
 *
 * NOTES
 *      These functions are stored in pg_amproc.  For each operator class
 *      defined for hash indexes, they compute the hash value of the argument.
 *
 *      Additional hash functions appear in /utils/adt/ files for various
 *      specialized datatypes.
 *
 *      It is expected that every bit of a hash function's 32-bit result is
 *      as random as every other; failure to ensure this is likely to lead
 *      to poor performance of hash joins, for example.  In most cases a hash
 *      function should use hash_any() or its variant hash_uint32().
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "utils/builtins.h"

#ifdef PGXC
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/date.h"
#include "utils/nabstime.h"
#endif

/*
 * Datatype-specific hash functions.
 *
 * These support both hash indexes and hash joins.
 *
 * NOTE: some of these are also used by catcache operations, without
 * any direct connection to hash indexes.  Also, the common hash_any
 * routine is also used by dynahash tables.
 */

/* Note: this is used for both "char" and boolean datatypes */
Datum
hashchar(PG_FUNCTION_ARGS)
{
    return hash_uint32((int32) PG_GETARG_CHAR(0));
}

Datum
hashcharextended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended((int32) PG_GETARG_CHAR(0), PG_GETARG_INT64(1));
}

Datum
hashint2(PG_FUNCTION_ARGS)
{
    return hash_uint32((int32) PG_GETARG_INT16(0));
}

Datum
hashint2extended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended((int32) PG_GETARG_INT16(0), PG_GETARG_INT64(1));
}

Datum
hashint4(PG_FUNCTION_ARGS)
{
    return hash_uint32(PG_GETARG_INT32(0));
}

Datum
hashint4extended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended(PG_GETARG_INT32(0), PG_GETARG_INT64(1));
}

Datum
hashint8(PG_FUNCTION_ARGS)
{
    /*
     * The idea here is to produce a hash value compatible with the values
     * produced by hashint4 and hashint2 for logically equal inputs; this is
     * necessary to support cross-type hash joins across these input types.
     * Since all three types are signed, we can xor the high half of the int8
     * value if the sign is positive, or the complement of the high half when
     * the sign is negative.
     */
    int64        val = PG_GETARG_INT64(0);
    uint32        lohalf = (uint32) val;
    uint32        hihalf = (uint32) (val >> 32);

    lohalf ^= (val >= 0) ? hihalf : ~hihalf;

    return hash_uint32(lohalf);
}

Datum
hashint8extended(PG_FUNCTION_ARGS)
{
	/* Same approach as hashint8 */
	int64		val = PG_GETARG_INT64(0);
	uint32		lohalf = (uint32) val;
	uint32		hihalf = (uint32) (val >> 32);

	lohalf ^= (val >= 0) ? hihalf : ~hihalf;

	return hash_uint32_extended(lohalf, PG_GETARG_INT64(1));
}

Datum
hashoid(PG_FUNCTION_ARGS)
{
    return hash_uint32((uint32) PG_GETARG_OID(0));
}

Datum
hashoidextended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended((uint32) PG_GETARG_OID(0), PG_GETARG_INT64(1));
}

Datum
hashenum(PG_FUNCTION_ARGS)
{
    return hash_uint32((uint32) PG_GETARG_OID(0));
}

Datum
hashenumextended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended((uint32) PG_GETARG_OID(0), PG_GETARG_INT64(1));
}

Datum
hashfloat4(PG_FUNCTION_ARGS)
{
    float4        key = PG_GETARG_FLOAT4(0);
    float8        key8;

    /*
     * On IEEE-float machines, minus zero and zero have different bit patterns
     * but should compare as equal.  We must ensure that they have the same
     * hash value, which is most reliably done this way:
     */
    if (key == (float4) 0)
        PG_RETURN_UINT32(0);

    /*
     * To support cross-type hashing of float8 and float4, we want to return
     * the same hash value hashfloat8 would produce for an equal float8 value.
     * So, widen the value to float8 and hash that.  (We must do this rather
     * than have hashfloat8 try to narrow its value to float4; that could fail
     * on overflow.)
     */
    key8 = key;

    return hash_any((unsigned char *) &key8, sizeof(key8));
}

Datum
hashfloat4extended(PG_FUNCTION_ARGS)
{
	float4		key = PG_GETARG_FLOAT4(0);
	uint64		seed = PG_GETARG_INT64(1);
	float8		key8;

	/* Same approach as hashfloat4 */
	if (key == (float4) 0)
		PG_RETURN_UINT64(seed);
	key8 = key;

	return hash_any_extended((unsigned char *) &key8, sizeof(key8), seed);
}

Datum
hashfloat8(PG_FUNCTION_ARGS)
{
    float8        key = PG_GETARG_FLOAT8(0);

    /*
     * On IEEE-float machines, minus zero and zero have different bit patterns
     * but should compare as equal.  We must ensure that they have the same
     * hash value, which is most reliably done this way:
     */
    if (key == (float8) 0)
        PG_RETURN_UINT32(0);

    return hash_any((unsigned char *) &key, sizeof(key));
}

Datum
hashfloat8extended(PG_FUNCTION_ARGS)
{
	float8		key = PG_GETARG_FLOAT8(0);
	uint64		seed = PG_GETARG_INT64(1);

	/* Same approach as hashfloat8 */
	if (key == (float8) 0)
		PG_RETURN_UINT64(seed);

	return hash_any_extended((unsigned char *) &key, sizeof(key), seed);
}

Datum
hashoidvector(PG_FUNCTION_ARGS)
{
    oidvector  *key = (oidvector *) PG_GETARG_POINTER(0);

    return hash_any((unsigned char *) key->values, key->dim1 * sizeof(Oid));
}

Datum
hashoidvectorextended(PG_FUNCTION_ARGS)
{
	oidvector  *key = (oidvector *) PG_GETARG_POINTER(0);

	return hash_any_extended((unsigned char *) key->values,
							 key->dim1 * sizeof(Oid),
							 PG_GETARG_INT64(1));
}

Datum
hashname(PG_FUNCTION_ARGS)
{
    char       *key = NameStr(*PG_GETARG_NAME(0));

    return hash_any((unsigned char *) key, strlen(key));
}

Datum
hashnameextended(PG_FUNCTION_ARGS)
{
	char	   *key = NameStr(*PG_GETARG_NAME(0));

	return hash_any_extended((unsigned char *) key, strlen(key),
							 PG_GETARG_INT64(1));
}

Datum
hashtext(PG_FUNCTION_ARGS)
{
    text       *key = PG_GETARG_TEXT_PP(0);
    Datum        result;

    /*
     * Note: this is currently identical in behavior to hashvarlena, but keep
     * it as a separate function in case we someday want to do something
     * different in non-C locales.  (See also hashbpchar, if so.)
     */
    result = hash_any((unsigned char *) VARDATA_ANY(key),
                      VARSIZE_ANY_EXHDR(key));

    /* Avoid leaking memory for toasted inputs */
    PG_FREE_IF_COPY(key, 0);

    return result;
}

Datum
hashtextextended(PG_FUNCTION_ARGS)
{
	text	   *key = PG_GETARG_TEXT_PP(0);
	Datum		result;

	/* Same approach as hashtext */
	result = hash_any_extended((unsigned char *) VARDATA_ANY(key),
							   VARSIZE_ANY_EXHDR(key),
							   PG_GETARG_INT64(1));

	PG_FREE_IF_COPY(key, 0);

	return result;
}

/*
 * hashvarlena() can be used for any varlena datatype in which there are
 * no non-significant bits, ie, distinct bitpatterns never compare as equal.
 */
Datum
hashvarlena(PG_FUNCTION_ARGS)
{
    struct varlena *key = PG_GETARG_VARLENA_PP(0);
    Datum        result;

    result = hash_any((unsigned char *) VARDATA_ANY(key),
                      VARSIZE_ANY_EXHDR(key));

    /* Avoid leaking memory for toasted inputs */
    PG_FREE_IF_COPY(key, 0);

    return result;
}

Datum
hashvarlenaextended(PG_FUNCTION_ARGS)
{
	struct varlena *key = PG_GETARG_VARLENA_PP(0);
	Datum		result;

	result = hash_any_extended((unsigned char *) VARDATA_ANY(key),
							   VARSIZE_ANY_EXHDR(key),
							   PG_GETARG_INT64(1));

	PG_FREE_IF_COPY(key, 0);

	return result;
}

/*
 * This hash function was written by Bob Jenkins
 * (bob_jenkins@burtleburtle.net), and superficially adapted
 * for PostgreSQL by Neil Conway. For more information on this
 * hash function, see http://burtleburtle.net/bob/hash/doobs.html,
 * or Bob's article in Dr. Dobb's Journal, Sept. 1997.
 *
 * In the current code, we have adopted Bob's 2006 update of his hash
 * function to fetch the data a word at a time when it is suitably aligned.
 * This makes for a useful speedup, at the cost of having to maintain
 * four code paths (aligned vs unaligned, and little-endian vs big-endian).
 * It also uses two separate mixing functions mix() and final(), instead
 * of a slower multi-purpose function.
 */

/* Get a bit mask of the bits set in non-uint32 aligned addresses */
#define UINT32_ALIGN_MASK (sizeof(uint32) - 1)

/* Rotate a uint32 value left by k bits - note multiple evaluation! */
#define rot(x,k) (((x)<<(k)) | ((x)>>(32-(k))))

/*----------
 * mix -- mix 3 32-bit values reversibly.
 *
 * This is reversible, so any information in (a,b,c) before mix() is
 * still in (a,b,c) after mix().
 *
 * If four pairs of (a,b,c) inputs are run through mix(), or through
 * mix() in reverse, there are at least 32 bits of the output that
 * are sometimes the same for one pair and different for another pair.
 * This was tested for:
 * * pairs that differed by one bit, by two bits, in any combination
 *     of top bits of (a,b,c), or in any combination of bottom bits of
 *     (a,b,c).
 * * "differ" is defined as +, -, ^, or ~^.  For + and -, I transformed
 *     the output delta to a Gray code (a^(a>>1)) so a string of 1's (as
 *     is commonly produced by subtraction) look like a single 1-bit
 *     difference.
 * * the base values were pseudorandom, all zero but one bit set, or
 *     all zero plus a counter that starts at zero.
 *
 * This does not achieve avalanche.  There are input bits of (a,b,c)
 * that fail to affect some output bits of (a,b,c), especially of a.  The
 * most thoroughly mixed value is c, but it doesn't really even achieve
 * avalanche in c.
 *
 * This allows some parallelism.  Read-after-writes are good at doubling
 * the number of bits affected, so the goal of mixing pulls in the opposite
 * direction from the goal of parallelism.  I did what I could.  Rotates
 * seem to cost as much as shifts on every machine I could lay my hands on,
 * and rotates are much kinder to the top and bottom bits, so I used rotates.
 *----------
 */
#define mix(a,b,c) \
{ \
  a -= c;  a ^= rot(c, 4);    c += b; \
  b -= a;  b ^= rot(a, 6);    a += c; \
  c -= b;  c ^= rot(b, 8);    b += a; \
  a -= c;  a ^= rot(c,16);    c += b; \
  b -= a;  b ^= rot(a,19);    a += c; \
  c -= b;  c ^= rot(b, 4);    b += a; \
}

/*----------
 * final -- final mixing of 3 32-bit values (a,b,c) into c
 *
 * Pairs of (a,b,c) values differing in only a few bits will usually
 * produce values of c that look totally different.  This was tested for
 * * pairs that differed by one bit, by two bits, in any combination
 *     of top bits of (a,b,c), or in any combination of bottom bits of
 *     (a,b,c).
 * * "differ" is defined as +, -, ^, or ~^.  For + and -, I transformed
 *     the output delta to a Gray code (a^(a>>1)) so a string of 1's (as
 *     is commonly produced by subtraction) look like a single 1-bit
 *     difference.
 * * the base values were pseudorandom, all zero but one bit set, or
 *     all zero plus a counter that starts at zero.
 *
 * The use of separate functions for mix() and final() allow for a
 * substantial performance increase since final() does not need to
 * do well in reverse, but is does need to affect all output bits.
 * mix(), on the other hand, does not need to affect all output
 * bits (affecting 32 bits is enough).  The original hash function had
 * a single mixing operation that had to satisfy both sets of requirements
 * and was slower as a result.
 *----------
 */
#define final(a,b,c) \
{ \
  c ^= b; c -= rot(b,14); \
  a ^= c; a -= rot(c,11); \
  b ^= a; b -= rot(a,25); \
  c ^= b; c -= rot(b,16); \
  a ^= c; a -= rot(c, 4); \
  b ^= a; b -= rot(a,14); \
  c ^= b; c -= rot(b,24); \
}

/*
 * hash_any() -- hash a variable-length key into a 32-bit value
 *        k        : the key (the unaligned variable-length array of bytes)
 *        len        : the length of the key, counting by bytes
 *
 * Returns a uint32 value.  Every bit of the key affects every bit of
 * the return value.  Every 1-bit and 2-bit delta achieves avalanche.
 * About 6*len+35 instructions. The best hash table sizes are powers
 * of 2.  There is no need to do mod a prime (mod is sooo slow!).
 * If you need less than 32 bits, use a bitmask.
 *
 * This procedure must never throw elog(ERROR); the ResourceOwner code
 * relies on this not to fail.
 *
 * Note: we could easily change this function to return a 64-bit hash value
 * by using the final values of both b and c.  b is perhaps a little less
 * well mixed than c, however.
 */
Datum
hash_any(register const unsigned char *k, register int keylen)
{// #lizard forgives
    register uint32 a,
                b,
                c,
                len;

    /* Set up the internal state */
    len = keylen;
    a = b = c = 0x9e3779b9 + len + 3923095;

    /* If the source pointer is word-aligned, we use word-wide fetches */
    if (((uintptr_t) k & UINT32_ALIGN_MASK) == 0)
    {
        /* Code path for aligned source data */
        register const uint32 *ka = (const uint32 *) k;

        /* handle most of the key */
        while (len >= 12)
        {
            a += ka[0];
            b += ka[1];
            c += ka[2];
            mix(a, b, c);
            ka += 3;
            len -= 12;
        }

        /* handle the last 11 bytes */
        k = (const unsigned char *) ka;
#ifdef WORDS_BIGENDIAN
        switch (len)
        {
            case 11:
                c += ((uint32) k[10] << 8);
                /* fall through */
            case 10:
                c += ((uint32) k[9] << 16);
                /* fall through */
            case 9:
                c += ((uint32) k[8] << 24);
                /* the lowest byte of c is reserved for the length */
                /* fall through */
            case 8:
                b += ka[1];
                a += ka[0];
                break;
            case 7:
                b += ((uint32) k[6] << 8);
                /* fall through */
            case 6:
                b += ((uint32) k[5] << 16);
                /* fall through */
            case 5:
                b += ((uint32) k[4] << 24);
                /* fall through */
            case 4:
                a += ka[0];
                break;
            case 3:
                a += ((uint32) k[2] << 8);
                /* fall through */
            case 2:
                a += ((uint32) k[1] << 16);
                /* fall through */
            case 1:
                a += ((uint32) k[0] << 24);
                /* case 0: nothing left to add */
        }
#else                            /* !WORDS_BIGENDIAN */
        switch (len)
        {
            case 11:
                c += ((uint32) k[10] << 24);
                /* fall through */
            case 10:
                c += ((uint32) k[9] << 16);
                /* fall through */
            case 9:
                c += ((uint32) k[8] << 8);
                /* the lowest byte of c is reserved for the length */
                /* fall through */
            case 8:
                b += ka[1];
                a += ka[0];
                break;
            case 7:
                b += ((uint32) k[6] << 16);
                /* fall through */
            case 6:
                b += ((uint32) k[5] << 8);
                /* fall through */
            case 5:
                b += k[4];
                /* fall through */
            case 4:
                a += ka[0];
                break;
            case 3:
                a += ((uint32) k[2] << 16);
                /* fall through */
            case 2:
                a += ((uint32) k[1] << 8);
                /* fall through */
            case 1:
                a += k[0];
                /* case 0: nothing left to add */
        }
#endif                            /* WORDS_BIGENDIAN */
    }
    else
    {
        /* Code path for non-aligned source data */

        /* handle most of the key */
        while (len >= 12)
        {
#ifdef WORDS_BIGENDIAN
            a += (k[3] + ((uint32) k[2] << 8) + ((uint32) k[1] << 16) + ((uint32) k[0] << 24));
            b += (k[7] + ((uint32) k[6] << 8) + ((uint32) k[5] << 16) + ((uint32) k[4] << 24));
            c += (k[11] + ((uint32) k[10] << 8) + ((uint32) k[9] << 16) + ((uint32) k[8] << 24));
#else                            /* !WORDS_BIGENDIAN */
            a += (k[0] + ((uint32) k[1] << 8) + ((uint32) k[2] << 16) + ((uint32) k[3] << 24));
            b += (k[4] + ((uint32) k[5] << 8) + ((uint32) k[6] << 16) + ((uint32) k[7] << 24));
            c += (k[8] + ((uint32) k[9] << 8) + ((uint32) k[10] << 16) + ((uint32) k[11] << 24));
#endif                            /* WORDS_BIGENDIAN */
            mix(a, b, c);
            k += 12;
            len -= 12;
        }

        /* handle the last 11 bytes */
#ifdef WORDS_BIGENDIAN
        switch (len)            /* all the case statements fall through */
        {
            case 11:
                c += ((uint32) k[10] << 8);
            case 10:
                c += ((uint32) k[9] << 16);
            case 9:
                c += ((uint32) k[8] << 24);
                /* the lowest byte of c is reserved for the length */
            case 8:
                b += k[7];
            case 7:
                b += ((uint32) k[6] << 8);
            case 6:
                b += ((uint32) k[5] << 16);
            case 5:
                b += ((uint32) k[4] << 24);
            case 4:
                a += k[3];
            case 3:
                a += ((uint32) k[2] << 8);
            case 2:
                a += ((uint32) k[1] << 16);
            case 1:
                a += ((uint32) k[0] << 24);
                /* case 0: nothing left to add */
        }
#else                            /* !WORDS_BIGENDIAN */
        switch (len)            /* all the case statements fall through */
        {
            case 11:
                c += ((uint32) k[10] << 24);
            case 10:
                c += ((uint32) k[9] << 16);
            case 9:
                c += ((uint32) k[8] << 8);
                /* the lowest byte of c is reserved for the length */
            case 8:
                b += ((uint32) k[7] << 24);
            case 7:
                b += ((uint32) k[6] << 16);
            case 6:
                b += ((uint32) k[5] << 8);
            case 5:
                b += k[4];
            case 4:
                a += ((uint32) k[3] << 24);
            case 3:
                a += ((uint32) k[2] << 16);
            case 2:
                a += ((uint32) k[1] << 8);
            case 1:
                a += k[0];
                /* case 0: nothing left to add */
        }
#endif                            /* WORDS_BIGENDIAN */
    }

    final(a, b, c);

    /* report the result */
    return UInt32GetDatum(c);
}

/*
 * hash_any_extended() -- hash into a 64-bit value, using an optional seed
 *		k		: the key (the unaligned variable-length array of bytes)
 *		len		: the length of the key, counting by bytes
 *		seed	: a 64-bit seed (0 means no seed)
 *
 * Returns a uint64 value.  Otherwise similar to hash_any.
 */
Datum
hash_any_extended(register const unsigned char *k, register int keylen,
				  uint64 seed)
{
	register uint32 a,
				b,
				c,
				len;

	/* Set up the internal state */
	len = keylen;
	a = b = c = 0x9e3779b9 + len + 3923095;

	/* If the seed is non-zero, use it to perturb the internal state. */
	if (seed != 0)
	{
		/*
		 * In essence, the seed is treated as part of the data being hashed,
		 * but for simplicity, we pretend that it's padded with four bytes of
		 * zeroes so that the seed constitutes a 12-byte chunk.
		 */
		a += (uint32) (seed >> 32);
		b += (uint32) seed;
		mix(a, b, c);
	}

	/* If the source pointer is word-aligned, we use word-wide fetches */
	if (((uintptr_t) k & UINT32_ALIGN_MASK) == 0)
	{
		/* Code path for aligned source data */
		register const uint32 *ka = (const uint32 *) k;

		/* handle most of the key */
		while (len >= 12)
		{
			a += ka[0];
			b += ka[1];
			c += ka[2];
			mix(a, b, c);
			ka += 3;
			len -= 12;
		}

		/* handle the last 11 bytes */
		k = (const unsigned char *) ka;
#ifdef WORDS_BIGENDIAN
		switch (len)
		{
			case 11:
				c += ((uint32) k[10] << 8);
				/* fall through */
			case 10:
				c += ((uint32) k[9] << 16);
				/* fall through */
			case 9:
				c += ((uint32) k[8] << 24);
				/* the lowest byte of c is reserved for the length */
				/* fall through */
			case 8:
				b += ka[1];
				a += ka[0];
				break;
			case 7:
				b += ((uint32) k[6] << 8);
				/* fall through */
			case 6:
				b += ((uint32) k[5] << 16);
				/* fall through */
			case 5:
				b += ((uint32) k[4] << 24);
				/* fall through */
			case 4:
				a += ka[0];
				break;
			case 3:
				a += ((uint32) k[2] << 8);
				/* fall through */
			case 2:
				a += ((uint32) k[1] << 16);
				/* fall through */
			case 1:
				a += ((uint32) k[0] << 24);
				/* case 0: nothing left to add */
		}
#else							/* !WORDS_BIGENDIAN */
		switch (len)
		{
			case 11:
				c += ((uint32) k[10] << 24);
				/* fall through */
			case 10:
				c += ((uint32) k[9] << 16);
				/* fall through */
			case 9:
				c += ((uint32) k[8] << 8);
				/* the lowest byte of c is reserved for the length */
				/* fall through */
			case 8:
				b += ka[1];
				a += ka[0];
				break;
			case 7:
				b += ((uint32) k[6] << 16);
				/* fall through */
			case 6:
				b += ((uint32) k[5] << 8);
				/* fall through */
			case 5:
				b += k[4];
				/* fall through */
			case 4:
				a += ka[0];
				break;
			case 3:
				a += ((uint32) k[2] << 16);
				/* fall through */
			case 2:
				a += ((uint32) k[1] << 8);
				/* fall through */
			case 1:
				a += k[0];
				/* case 0: nothing left to add */
		}
#endif							/* WORDS_BIGENDIAN */
	}
	else
	{
		/* Code path for non-aligned source data */

		/* handle most of the key */
		while (len >= 12)
		{
#ifdef WORDS_BIGENDIAN
			a += (k[3] + ((uint32) k[2] << 8) + ((uint32) k[1] << 16) + ((uint32) k[0] << 24));
			b += (k[7] + ((uint32) k[6] << 8) + ((uint32) k[5] << 16) + ((uint32) k[4] << 24));
			c += (k[11] + ((uint32) k[10] << 8) + ((uint32) k[9] << 16) + ((uint32) k[8] << 24));
#else							/* !WORDS_BIGENDIAN */
			a += (k[0] + ((uint32) k[1] << 8) + ((uint32) k[2] << 16) + ((uint32) k[3] << 24));
			b += (k[4] + ((uint32) k[5] << 8) + ((uint32) k[6] << 16) + ((uint32) k[7] << 24));
			c += (k[8] + ((uint32) k[9] << 8) + ((uint32) k[10] << 16) + ((uint32) k[11] << 24));
#endif							/* WORDS_BIGENDIAN */
			mix(a, b, c);
			k += 12;
			len -= 12;
		}

		/* handle the last 11 bytes */
#ifdef WORDS_BIGENDIAN
		switch (len)			/* all the case statements fall through */
		{
			case 11:
				c += ((uint32) k[10] << 8);
			case 10:
				c += ((uint32) k[9] << 16);
			case 9:
				c += ((uint32) k[8] << 24);
				/* the lowest byte of c is reserved for the length */
			case 8:
				b += k[7];
			case 7:
				b += ((uint32) k[6] << 8);
			case 6:
				b += ((uint32) k[5] << 16);
			case 5:
				b += ((uint32) k[4] << 24);
			case 4:
				a += k[3];
			case 3:
				a += ((uint32) k[2] << 8);
			case 2:
				a += ((uint32) k[1] << 16);
			case 1:
				a += ((uint32) k[0] << 24);
				/* case 0: nothing left to add */
		}
#else							/* !WORDS_BIGENDIAN */
		switch (len)			/* all the case statements fall through */
		{
			case 11:
				c += ((uint32) k[10] << 24);
			case 10:
				c += ((uint32) k[9] << 16);
			case 9:
				c += ((uint32) k[8] << 8);
				/* the lowest byte of c is reserved for the length */
			case 8:
				b += ((uint32) k[7] << 24);
			case 7:
				b += ((uint32) k[6] << 16);
			case 6:
				b += ((uint32) k[5] << 8);
			case 5:
				b += k[4];
			case 4:
				a += ((uint32) k[3] << 24);
			case 3:
				a += ((uint32) k[2] << 16);
			case 2:
				a += ((uint32) k[1] << 8);
			case 1:
				a += k[0];
				/* case 0: nothing left to add */
		}
#endif							/* WORDS_BIGENDIAN */
	}

	final(a, b, c);

	/* report the result */
	PG_RETURN_UINT64(((uint64) b << 32) | c);
}

/*
 * hash_uint32() -- hash a 32-bit value to a 32-bit value
 *
 * This has the same result as
 *        hash_any(&k, sizeof(uint32))
 * but is faster and doesn't force the caller to store k into memory.
 */
Datum
hash_uint32(uint32 k)
{
    register uint32 a,
                b,
                c;

    a = b = c = 0x9e3779b9 + (uint32) sizeof(uint32) + 3923095;
    a += k;

    final(a, b, c);

    /* report the result */
    return UInt32GetDatum(c);
}

/*
 * hash_uint32_extended() -- hash a 32-bit value to a 64-bit value, with a seed
 *
 * Like hash_uint32, this is a convenience function.
 */
Datum
hash_uint32_extended(uint32 k, uint64 seed)
{
   register uint32 a,
               b,
               c;

   a = b = c = 0x9e3779b9 + (uint32) sizeof(uint32) + 3923095;

   if (seed != 0)
   {
       a += (uint32) (seed >> 32);
       b += (uint32) seed;
       mix(a, b, c);
   }

   a += k;

   final(a, b, c);

   /* report the result */
   PG_RETURN_UINT64(((uint64) b << 32) | c);
}

#ifdef PGXC
/*
 * compute_hash()
 * Generic hash function for all datatypes
 */
Datum
compute_hash(Oid type, Datum value, char locator)
{// #lizard forgives
    int16    tmp16;
    int32    tmp32;
    int64    tmp64;
    Oid        tmpoid;
    char    tmpch;

    switch (type)
    {
        case INT8OID:
            /* This gives added advantage that
             *    a = 8446744073709551359
             * and    a = 8446744073709551359::int8 both work*/
            tmp64 = DatumGetInt64(value);
#ifdef _MIGRATE_
            if (locator == LOCATOR_TYPE_HASH || locator == LOCATOR_TYPE_SHARD)
#else
            if (locator == LOCATOR_TYPE_HASH)
#endif
                return DirectFunctionCall1(hashint8, value);
            return tmp64;
        case INT2OID:
            tmp16 = DatumGetInt16(value);
#ifdef _MIGRATE_
            if (locator == LOCATOR_TYPE_HASH || locator == LOCATOR_TYPE_SHARD)
#else            
            if (locator == LOCATOR_TYPE_HASH)
#endif
                return DirectFunctionCall1(hashint2, tmp16);
            return tmp16;
        case OIDOID:
            tmpoid = DatumGetObjectId(value);
#ifdef _MIGRATE_
            if (locator == LOCATOR_TYPE_HASH || locator == LOCATOR_TYPE_SHARD)
#else            
            if (locator == LOCATOR_TYPE_HASH)
#endif

                return DirectFunctionCall1(hashoid, tmpoid);
            return tmpoid;
        case INT4OID:
            tmp32 = DatumGetInt32(value);
#ifdef _MIGRATE_
            if (locator == LOCATOR_TYPE_HASH || locator == LOCATOR_TYPE_SHARD)
#else            
            if (locator == LOCATOR_TYPE_HASH)
#endif

                return DirectFunctionCall1(hashint4, tmp32);
            return tmp32;
        case BOOLOID:
            tmpch = DatumGetBool(value);
#ifdef _MIGRATE_
            if (locator == LOCATOR_TYPE_HASH || locator == LOCATOR_TYPE_SHARD)
#else            
            if (locator == LOCATOR_TYPE_HASH)
#endif

                return DirectFunctionCall1(hashchar, tmpch);
            return tmpch;

        case CHAROID:
            return DirectFunctionCall1(hashchar, value);
        case NAMEOID:
            return DirectFunctionCall1(hashname, value);

        case VARCHAROID:
        case TEXTOID:
#ifdef _PG_ORCL_
        case VARCHAR2OID:
        case NVARCHAR2OID:
#endif
            return DirectFunctionCall1(hashtext, value);

        case OIDVECTOROID:
            return DirectFunctionCall1(hashoidvector, value);
        case FLOAT4OID:
            return DirectFunctionCall1(hashfloat4, value);
        case FLOAT8OID:
            return DirectFunctionCall1(hashfloat8, value);

        case ABSTIMEOID:
            tmp32 = DatumGetAbsoluteTime(value);
#ifdef _MIGRATE_
            if (locator == LOCATOR_TYPE_HASH || locator == LOCATOR_TYPE_SHARD)
#else            
            if (locator == LOCATOR_TYPE_HASH)
#endif
                return DirectFunctionCall1(hashint4, tmp32);
            return tmp32;
        case RELTIMEOID:
            tmp32 = DatumGetRelativeTime(value);
#ifdef _MIGRATE_
            if (locator == LOCATOR_TYPE_HASH || locator == LOCATOR_TYPE_SHARD)
#else            
            if (locator == LOCATOR_TYPE_HASH)
#endif
                return DirectFunctionCall1(hashint4, tmp32);
            return tmp32;
        case CASHOID:
            return DirectFunctionCall1(hashint8, value);

        case BPCHAROID:
            return DirectFunctionCall1(hashbpchar, value);
        case BYTEAOID:
            return DirectFunctionCall1(hashvarlena, value);

        case DATEOID:
            tmp32 = DatumGetDateADT(value);
#ifdef _MIGRATE_
            if (locator == LOCATOR_TYPE_HASH || locator == LOCATOR_TYPE_SHARD)
#else            
            if (locator == LOCATOR_TYPE_HASH)
#endif
                return DirectFunctionCall1(hashint4, tmp32);
            return tmp32;
        case TIMEOID:
            return DirectFunctionCall1(time_hash, value);
        case TIMESTAMPOID:
            return DirectFunctionCall1(timestamp_hash, value);
        case TIMESTAMPTZOID:
            return DirectFunctionCall1(timestamp_hash, value);
        case INTERVALOID:
            return DirectFunctionCall1(interval_hash, value);
        case TIMETZOID:
            return DirectFunctionCall1(timetz_hash, value);

        case NUMERICOID:
            return DirectFunctionCall1(hash_numeric, value);
#ifdef __TBASE__
		case JSONBOID:
		    return DirectFunctionCall1(jsonb_hash, value);
#endif
        default:
            ereport(ERROR,(errmsg("Unhandled datatype:%d for modulo or hash distribution in compute_hash", type)));
    }
    /* Control should not come here. */
    ereport(ERROR,(errmsg("Unhandled datatype for modulo or hash distribution\n")));
    /* Keep compiler silent */
    return (Datum)0;
}


/*
 * get_compute_hash_function
 * Get hash function name depending on the hash type.
 * For some cases of hash or modulo distribution, a function might
 * be required or not.
 */
char *
get_compute_hash_function(Oid type, char locator)
{// #lizard forgives
    switch (type)
    {
        case INT8OID:
            if (locator == LOCATOR_TYPE_HASH)
                return "hashint8";
            return NULL;
        case INT2OID:
            if (locator == LOCATOR_TYPE_HASH)
                return "hashint2";
            return NULL;
        case OIDOID:
            if (locator == LOCATOR_TYPE_HASH)
                return "hashoid";
            return NULL;
        case DATEOID:
        case INT4OID:
            if (locator == LOCATOR_TYPE_HASH)
                return "hashint4";
            return NULL;
        case BOOLOID:
            if (locator == LOCATOR_TYPE_HASH)
                return "hashchar";
            return NULL;
        case CHAROID:
            return "hashchar";
        case NAMEOID:
            return "hashname";
        case VARCHAROID:
        case TEXTOID:
#ifdef _PG_ORCL_
        case VARCHAR2OID:
        case NVARCHAR2OID:
#endif
            return "hashtext";
        case OIDVECTOROID:
            return "hashoidvector";
        case FLOAT4OID:
            return "hashfloat4";
        case FLOAT8OID:
            return "hashfloat8";
        case RELTIMEOID:
        case ABSTIMEOID:
            if (locator == LOCATOR_TYPE_HASH)
                return "hashint4";
            return NULL;
        case CASHOID:
                return "hashint8";
        case BPCHAROID:
            return "hashbpchar";
        case BYTEAOID:
            return "hashvarlena";
        case TIMEOID:
            return "time_hash";
        case TIMESTAMPOID:
        case TIMESTAMPTZOID:
            return "timestamp_hash";
        case INTERVALOID:
            return "interval_hash";
        case TIMETZOID:
            return "timetz_hash";
        case NUMERICOID:
            return "hash_numeric";
#ifdef __TBASE__
		case JSONBOID:
		    return "jsonb_hash";
#endif
        default:
            ereport(ERROR,(errmsg("Unhandled datatype:%d for modulo or hash distribution in get_compute_hash_function", type)));
    }

    /* Keep compiler quiet */
    return NULL;
}
#endif
