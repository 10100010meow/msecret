
#include <assert.h>
#include <stdbool.h>
#include "msecret.h"
#include "lkdf.h"
#include "hmac_sha/hmac_sha256.h"
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>

static uint8_t
enclosing_mask_uint8(uint8_t x) {
	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	return x;
}

void
MSECRET_CalcKeySelector(
	MSECRET_KeySelector keySelector_out,
	const uint8_t *salt, size_t salt_size,
	const char *info, size_t info_size
) {
	if ((info != NULL) && (info_size == 0)) {
		info_size = strlen(info);
	}
	return LKDF_SHA256_CalcKeySelector(
		keySelector_out,
		salt, salt_size,
		(const uint8_t*)info, info_size
	);
}

void MSECRET_Extract_Bytes(
	uint8_t *key_out, size_t key_size,
	const MSECRET_KeySelector key_selector,
	const uint8_t *ikm, size_t ikm_size
) {
	LKDF_SHA256_Extract(
		key_out, key_size,
		key_selector,
		ikm, ikm_size
	);
}

void
MSECRET_Extract_Integer(
	uint8_t *key_out,
	const uint8_t *max_in, size_t mod_size,
	const MSECRET_KeySelector key_selector,
	const uint8_t *ikm, size_t ikm_size
) {
	MSECRET_KeySelector new_key_selector;

	// Copy over the key selector. We will end up
	// mutating this, possibly multiple times to
	// satisfy the maximum.
	memcpy(new_key_selector, key_selector, sizeof(MSECRET_KeySelector));

	// Skip any leading zero bytes in the modulous
	while (mod_size && (*max_in == 0)) {
		*key_out++ = 0;
		max_in++;
		mod_size--;
	}

	// Search for a key which satisfies the modulous
	// We can end up attempting this multiple times
	// in order to satisfy the modulus. This ensures
	// that we have a uniform distribution.
	do {
		MSECRET_CalcKeySelector(
			new_key_selector,
			new_key_selector, sizeof(MSECRET_KeySelector),
			(const char*)max_in, mod_size
		);

		MSECRET_Extract_Bytes(
			key_out, mod_size,
			new_key_selector,
			ikm, ikm_size
		);

		// Mask the unnecessary bits of the first
		// byte. This makes the search faster.
		key_out[0] &= enclosing_mask_uint8(max_in[0]);

	} while( memcmp(key_out, max_in, mod_size) > 0 );
}

void
MSECRET_Extract_Integer_BN(
	BIGNUM *val,
	const BIGNUM *max,
	const MSECRET_KeySelector key_selector,
	const uint8_t *ikm, size_t ikm_size
) {
	int size = BN_num_bytes(max);
	uint8_t val_bytes[size];
	uint8_t max_bytes[size];
	BN_bn2bin(max, max_bytes);
	MSECRET_Extract_Integer(
		val_bytes,
		max_bytes,
		size,
		key_selector,
		ikm,ikm_size
	);
	BN_bin2bn(val_bytes, size, val);
}

int
MSECRET_Extract_Prime_BN(
	BIGNUM *prime,
	int bit_length,
	const MSECRET_KeySelector key_selector,
	const uint8_t *ikm, size_t ikm_size
) {
	int ret = kMSECRET_FAILED;
	MSECRET_KeySelector new_key_selector;
	HMAC_SHA256_CTX hmac;
	BN_CTX *ctx = BN_CTX_new();
	BIGNUM *max = BN_CTX_get(ctx);

	if (bit_length <= 4) {
		fprintf(stderr,"MSECRET_Extract_Prime_BN: Invalid bit length (%d)\n", bit_length);
		goto bail;
	}

	MSECRET_CalcKeySelector(
		new_key_selector,
		key_selector, sizeof(MSECRET_KeySelector),
		"Prime_v1", 0
	);

	// Set our target strength as 2^bit_length-1
	BN_set_bit(max, bit_length);
	BN_sub_word(max, 1);

	// Calculate our starting point.
	MSECRET_Extract_Integer_BN(
		prime,
		max,
		new_key_selector,
		ikm, ikm_size
	);

	// Make sure our starting point is odd.
	BN_set_bit(prime, 0);

	// Make sure the most-significant two bits are set.
	// This ensures the resulting prime is large.
	BN_set_bit(prime, bit_length-1);
	BN_set_bit(prime, bit_length-2);

	// Start looping through candidates.
	for (
		; !BN_is_prime_fasttest(prime, BN_prime_checks, NULL, ctx, NULL, 1)
		; BN_add_word(prime, 2)
	) {
		// All the work is done in the for() expression above
	}

	ret = kMSECRET_OK;

bail:

	BN_CTX_free(ctx);

	return ret;
}

int
MSECRET_Extract_RSA(
	RSA *rsa,
	int mod_length,
	const MSECRET_KeySelector key_selector,
	const uint8_t *ikm, size_t ikm_size
) {
	/* This implementation is largely based on the implementation
	 * from OpenSSL's `RSA_generate_key()` method, as seen here:
	 *
	 * https://opensource.apple.com/source/OpenSSL097/OpenSSL097-16/openssl/crypto/rsa/rsa_gen.c
	 */

	int ret = kMSECRET_FAILED;
	MSECRET_KeySelector prime_key_selector;
	int bitsp, bitsq;
	BN_CTX *ctx = BN_CTX_new();
	BIGNUM *r0 = BN_CTX_get(ctx);
	BIGNUM *r1 = BN_CTX_get(ctx);
	BIGNUM *r2 = BN_CTX_get(ctx);

	if (mod_length <= 0) {
		fprintf(stderr,"MSECRET_Extract_RSA: Invalid mod length (%d)\n", mod_length);
		goto bail;
	}

	bitsp = (mod_length + 1) / 2;
	bitsq = mod_length - bitsp;

	if (rsa->p == NULL) {
		rsa->p = BN_new();
	}

	if (rsa->q == NULL) {
		rsa->q = BN_new();
	}

	if (rsa->e == NULL) {
		rsa->e = BN_new();
	}

	// We always use 65537 as `e`.
	BN_set_word(rsa->e, RSA_F4);

	// Copy over the key selector. We will end up
	// mutating this, possibly multiple times to
	// satisfy the maximum.
	memcpy(prime_key_selector, key_selector, sizeof(MSECRET_KeySelector));

	// Calculate the first prime.
	do {
		// Mutate the key selector.
		MSECRET_CalcKeySelector(
			prime_key_selector,
			prime_key_selector, sizeof(MSECRET_KeySelector),
			"RSA_v1", 0
		);

		// Extract a prime number.
		ret = MSECRET_Extract_Prime_BN(
			rsa->p,
			bitsp,
			prime_key_selector,
			ikm, ikm_size
		);

		if (ret != kMSECRET_OK) {
			goto bail;
		}

		// Verify that the prime is suitable.
		// If it isn't, we try again.
		BN_sub(r0, rsa->p, BN_value_one());
		BN_gcd(r1, r0, rsa->e, ctx);
	} while (!BN_is_one(r1));

	// Calculate the second prime.
	do {
		// Mutate the key selector.
		MSECRET_CalcKeySelector(
			prime_key_selector,
			prime_key_selector, sizeof(MSECRET_KeySelector),
			"RSA_v1", 0
		);

		// Extract a prime number.
		ret = MSECRET_Extract_Prime_BN(
			rsa->q,
			bitsq,
			prime_key_selector,
			ikm, ikm_size
		);

		if (ret != kMSECRET_OK) {
			goto bail;
		}

		// Verify that the prime is suitable.
		// If it isn't, we try again.
		BN_sub(r0, rsa->q, BN_value_one());
		BN_gcd(r1, r0, rsa->e, ctx);
	} while (!BN_is_one(r1));

	assert(BN_cmp(rsa->p, rsa->q) != 0);

	// P should be the larger of the two, by convention.
	if (BN_cmp(rsa->p, rsa->q) < 0)
	{
		BIGNUM *tmp=rsa->p;
		rsa->p=rsa->q;
		rsa->q=tmp;
	}

	// Calculate N
	if (rsa->n == NULL) {
		rsa->n = BN_new();
	}
	BN_mul(rsa->n, rsa->p, rsa->q, ctx);

	// Calculate D
	if (rsa->d == NULL) {
		rsa->d = BN_new();
	}
	BN_sub(r0, rsa->n, rsa->p);
	BN_sub(r1, r0, rsa->q);
	BN_add(r0, r1, BN_value_one());
	BN_mod_inverse(rsa->d, rsa->e, r0, ctx);

	// Calculate DMP1
	if (rsa->dmp1 == NULL) {
		rsa->dmp1 = BN_new();
	}
	BN_sub(r1, rsa->p, BN_value_one());
	BN_mod(rsa->dmp1,rsa->d,r1,ctx);

	// Calculate DMQ1
	if (rsa->dmq1 == NULL) {
		rsa->dmq1 = BN_new();
	}
	BN_sub(r2, rsa->q, BN_value_one());
	BN_mod(rsa->dmq1,rsa->d,r2,ctx);

	// Calculate IQMP
	if (rsa->iqmp == NULL) {
		rsa->iqmp = BN_new();
	}
	BN_mod_inverse(rsa->iqmp,rsa->q,rsa->p,ctx);

	ret = kMSECRET_OK;

bail:
	BN_CTX_free(ctx);
	return ret;
}

int MSECRET_Extract_EC_KEY(
	EC_KEY *ec_key_out,
	const MSECRET_KeySelector key_selector,
	const uint8_t *ikm, size_t ikm_size
) {
	int ret = kMSECRET_FAILED;
	BN_CTX *ctx = BN_CTX_new();
	const EC_GROUP *group = EC_KEY_get0_group(ec_key_out);
	BIGNUM *order = BN_CTX_get(ctx);
	BIGNUM *privateKey = BN_CTX_get(ctx);
	MSECRET_KeySelector new_key_selector;
	EC_POINT *ec_pub_key = NULL;

	if (group == NULL) {
		fprintf(stderr,"MSECRET_Extract_EC_KEY: No group specified\n");
		goto bail;
	}

	if (!EC_GROUP_get_order(group, order, ctx)) {
		fprintf(stderr,"MSECRET_Extract_EC_KEY: Unable to extract order\n");
		goto bail;
	}

	MSECRET_CalcKeySelector(
		new_key_selector,
		key_selector, sizeof(MSECRET_KeySelector),
		"EC_v1", 0
	);

	MSECRET_Extract_Integer_BN(
		privateKey,
		order,
		new_key_selector,
		ikm, ikm_size
	);

	EC_KEY_set_private_key(ec_key_out, privateKey);

	ec_pub_key = EC_POINT_new(group);
	if (!EC_POINT_mul(group, ec_pub_key, privateKey, NULL, NULL, NULL)) {
		fprintf(stderr,"Error at EC_POINT_mul.\n");
		goto bail;
	}

	EC_KEY_set_public_key(ec_key_out, ec_pub_key);

	if (!EC_KEY_check_key(ec_key_out)) {
		fprintf(stderr,"Error at EC_KEY_check_key.\n");
		goto bail;
	}

	ret = kMSECRET_OK;

bail:
	if (ec_pub_key != NULL) {
		EC_POINT_free(ec_pub_key);
	}

	BN_CTX_free(ctx);

	return ret;
}

#if MSECRET_UNIT_TEST

int
hex_dump(FILE* file, const uint8_t *data, size_t data_len)
{
	int ret = 0;
	while (data_len > 0) {
		int i = 0;
		if (data_len == 1) {
			i = fprintf(file,"%02X",*data);
		} else {
			i = fprintf(file, "%02X ",*data);
		}

		if (i < 0) {
			ret = i;
		}

		if (i <= 0) {
			break;
		}

		ret += i;
		data_len--;
		data++;
	}
	return ret;
}

int
main(void) {
	{
		static const uint8_t master_secret[512] = {0};
		static const char info[] = "MSECRET Test Vector";

		uint8_t key_out[16] = { 0 };
		uint8_t max_in[16] = { 0, 0x08, 0xFF };
		MSECRET_KeySelector key_selector;

		MSECRET_CalcKeySelector(
			key_selector,
			NULL, 0,
			info, 0
		);

		MSECRET_Extract_Integer(
			key_out, max_in, sizeof(max_in),
			key_selector,
			master_secret, sizeof(master_secret)
		);

		fprintf(stdout, "max_in  = ");
		hex_dump(stdout, max_in, sizeof(max_in));
		fprintf(stdout, "\n");
		fprintf(stdout, "key_out = ");
		hex_dump(stdout, key_out, sizeof(key_out));
		fprintf(stdout, "\n");
	}
}

#endif
