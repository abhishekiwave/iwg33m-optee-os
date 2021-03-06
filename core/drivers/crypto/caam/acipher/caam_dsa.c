// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2019-2020 NXP
 *
 * Implementation of DSA functions
 */
#include <caam_acipher.h>
#include <caam_common.h>
#include <caam_hal_ctrl.h>
#include <caam_jr.h>
#include <caam_utils_mem.h>
#include <caam_utils_status.h>
#include <drvcrypt.h>
#include <drvcrypt_acipher.h>
#include <mm/core_memprot.h>
#include <tee/cache.h>
#include <string.h>

#include "local.h"

/*
 * Definition of the local DSA Keypair
 *   Domain Parameters (p, q, g)
 *   Private Key format (x)
 *   Public Key format (y)
 */
struct caam_dsa_keypair {
	struct caambuf g; /* Generator */
	struct caambuf p; /* Prime Number (L bits) */
	struct caambuf q; /* Subprime Number (N bits) */
	struct caambuf x; /* Private key */
	struct caambuf y; /* Public key */
};

/*
 * Free local DSA keypair
 *
 * @key  DSA keypair
 */
static void do_keypair_free(struct caam_dsa_keypair *key)
{
	caam_free_buf(&key->g);
	caam_free_buf(&key->p);
	caam_free_buf(&key->q);
	caam_free_buf(&key->x);
	caam_free_buf(&key->y);
}

/*
 * If all DSA parameters p, q and g are present, convert them from bignumbers
 * to local buffers (via keypair object).
 * Otherwise generate them.
 *
 * @outkey    [out] Output keypair in local format
 * @inkey     Input key in TEE Crypto format
 * @l_bytes   Prime p size in bytes
 * @n_bytes   Subprime q size in bytes
 */
static TEE_Result get_keypair_domain_params(struct caam_dsa_keypair *outkey,
					    const struct dsa_keypair *key,
					    size_t l_bytes, size_t n_bytes)
{
	enum caam_status retstatus = CAAM_OUT_MEMORY;
	size_t in_q_size = 0;
	size_t in_p_size = 0;
	size_t in_g_size = 0;
	struct prime_dsa prime = {};

	DSA_TRACE("DSA conv key param (p, g) of %zu bytes and (q) of %zu bytes",
		  l_bytes, n_bytes);

	retstatus = caam_calloc_buf(&outkey->q, n_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return TEE_ERROR_OUT_OF_MEMORY;

	retstatus = caam_calloc_buf(&outkey->g, l_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return TEE_ERROR_OUT_OF_MEMORY;

	retstatus = caam_calloc_buf(&outkey->p, l_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return TEE_ERROR_OUT_OF_MEMORY;

	/*
	 * Get all inputs parameters size, if one of them is not
	 * define generate new parameters
	 */
	in_g_size = crypto_bignum_num_bytes(key->g);
	in_p_size = crypto_bignum_num_bytes(key->p);
	in_q_size = crypto_bignum_num_bytes(key->q);

	if (!in_q_size || !in_g_size || !in_p_size) {
		/* Generate DSA parameters: Generator G and Primes P/Q */
		prime.g = &outkey->g;
		prime.p = &outkey->p;
		prime.q = &outkey->q;

		retstatus = caam_prime_dsa_gen(&prime);
		DSA_TRACE("Generate G and Primes P/Q returned 0x%" PRIx32,
			  retstatus);

		if (retstatus != CAAM_NO_ERROR)
			return TEE_ERROR_GENERIC;

		/* Copy Generated DSA Parameter */
		crypto_bignum_bin2bn(outkey->q.data, outkey->q.length, key->q);
		crypto_bignum_bin2bn(outkey->g.data, outkey->g.length, key->g);
		crypto_bignum_bin2bn(outkey->p.data, outkey->p.length, key->p);

	} else {
		DSA_TRACE("Prime Q is defined");

		crypto_bignum_bn2bin(key->q,
				     outkey->q.data + n_bytes - in_q_size);
		cache_operation(TEE_CACHECLEAN, outkey->q.data,
				outkey->q.length);

		DSA_TRACE("Prime G is defined");
		crypto_bignum_bn2bin(key->g,
				     outkey->g.data + l_bytes - in_g_size);
		cache_operation(TEE_CACHECLEAN, outkey->g.data,
				outkey->g.length);

		DSA_TRACE("Prime P is defined");
		crypto_bignum_bn2bin(key->p,
				     outkey->p.data + l_bytes - in_p_size);
		cache_operation(TEE_CACHECLEAN, outkey->p.data,
				outkey->p.length);
	}

	return TEE_SUCCESS;
}

/*
 * Convert Crypto DSA Private Key to local Keypair Key
 * Ensure Key is push in physical memory
 *
 * @outkey    [out] Output keypair in local format
 * @inkey     Input key in TEE Crypto format
 * @l_bytes   Prime p size in bytes
 * @n_bytes   Subprime q size in bytes
 */
static enum caam_status do_keypriv_conv(struct caam_dsa_keypair *outkey,
					const struct dsa_keypair *inkey,
					size_t l_bytes, size_t n_bytes)
{
	enum caam_status retstatus = CAAM_OUT_MEMORY;
	size_t field_size = 0;

	DSA_TRACE("DSA Convert Key Private size l=%zu bytes, n=%zu bytes",
		  l_bytes, n_bytes);

	/* Generator */
	retstatus = caam_calloc_buf(&outkey->g, l_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return CAAM_OUT_MEMORY;

	/* Get the number of bytes of g to pad with 0's */
	field_size = crypto_bignum_num_bytes(inkey->g);
	crypto_bignum_bn2bin(inkey->g, outkey->g.data + l_bytes - field_size);

	/* Prime Number Modulus */
	retstatus = caam_calloc_buf(&outkey->p, l_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return CAAM_OUT_MEMORY;

	/* Get the number of bytes of p to pad with 0's */
	field_size = crypto_bignum_num_bytes(inkey->p);
	crypto_bignum_bn2bin(inkey->p, outkey->p.data + l_bytes - field_size);

	/* Subprime Number Modulus */
	retstatus = caam_calloc_buf(&outkey->q, n_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return CAAM_OUT_MEMORY;

	/* Get the number of bytes of q to pad with 0's */
	field_size = crypto_bignum_num_bytes(inkey->q);
	crypto_bignum_bn2bin(inkey->q, outkey->q.data + n_bytes - field_size);

	/* Private key is only scalar x of n bytes */
	retstatus = caam_calloc_buf(&outkey->x, n_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return CAAM_OUT_MEMORY;

	/* Get the number of bytes of x to pad with 0's */
	field_size = crypto_bignum_num_bytes(inkey->x);
	crypto_bignum_bn2bin(inkey->x, outkey->x.data + n_bytes - field_size);

	cache_operation(TEE_CACHECLEAN, outkey->g.data, outkey->g.length);
	cache_operation(TEE_CACHECLEAN, outkey->p.data, outkey->p.length);
	cache_operation(TEE_CACHECLEAN, outkey->q.data, outkey->q.length);
	cache_operation(TEE_CACHECLEAN, outkey->x.data, outkey->x.length);

	return CAAM_NO_ERROR;
}

/*
 * Convert Crypto DSA Public Key to local DSA Keypair Key
 * Ensure Key is push in physical memory
 *
 * @outkey    [out] Output keypair in local format
 * @inkey     Input key in TEE Crypto format
 * @l_bytes   Prime p size in bytes
 * @n_bytes   Subprime q size in bytes
 */
static enum caam_status do_keypub_conv(struct caam_dsa_keypair *outkey,
				       const struct dsa_public_key *inkey,
				       size_t l_bytes, size_t n_bytes)
{
	enum caam_status retstatus = CAAM_OUT_MEMORY;
	size_t field_size = 0;

	DSA_TRACE("DSA Convert Public Key size l=%zu bytes, n=%zu bytes",
		  l_bytes, n_bytes);

	/* Generator */
	retstatus = caam_calloc_buf(&outkey->g, l_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return CAAM_OUT_MEMORY;

	/* Get the number of bytes of g to pad with 0's */
	field_size = crypto_bignum_num_bytes(inkey->g);
	crypto_bignum_bn2bin(inkey->g, outkey->g.data + l_bytes - field_size);

	/* Prime Number Modulus */
	retstatus = caam_calloc_buf(&outkey->p, l_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return CAAM_OUT_MEMORY;

	/* Get the number of bytes of p to pad with 0's */
	field_size = crypto_bignum_num_bytes(inkey->p);
	crypto_bignum_bn2bin(inkey->p, outkey->p.data + l_bytes - field_size);

	/* Subprime Number Modulus */
	retstatus = caam_calloc_buf(&outkey->q, n_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return CAAM_OUT_MEMORY;

	/* Get the number of bytes of q to pad with 0's */
	field_size = crypto_bignum_num_bytes(inkey->q);
	crypto_bignum_bn2bin(inkey->q, outkey->q.data + n_bytes - field_size);

	/* Public key is only scalar y of l bytes */
	retstatus = caam_calloc_buf(&outkey->y, l_bytes);
	if (retstatus != CAAM_NO_ERROR)
		return CAAM_OUT_MEMORY;

	/* Get the number of bytes of y to pad with 0's */
	field_size = crypto_bignum_num_bytes(inkey->y);
	crypto_bignum_bn2bin(inkey->y, outkey->y.data + l_bytes - field_size);

	cache_operation(TEE_CACHECLEAN, outkey->g.data, outkey->g.length);
	cache_operation(TEE_CACHECLEAN, outkey->p.data, outkey->p.length);
	cache_operation(TEE_CACHECLEAN, outkey->q.data, outkey->q.length);
	cache_operation(TEE_CACHECLEAN, outkey->y.data, outkey->y.length);

	return CAAM_NO_ERROR;
}

/*
 * Allocate a TEE DSA keypair.
 *
 * @key        Keypair
 * @l_bits     L bits size (prime p size)
 * @n_bits     N bits size (subprime q size)
 */
static TEE_Result do_allocate_keypair(struct dsa_keypair *key, size_t l_bits,
				      size_t n_bits)
{
	DSA_TRACE("DSA allocate Keypair of L=%zu bits and N=%zu bits", l_bits,
		  n_bits);

	/* Initialize the key fields to NULL */
	memset(key, 0, sizeof(*key));

	/* Allocate Generator Scalar */
	key->g = crypto_bignum_allocate(l_bits);
	if (!key->g)
		goto err_alloc_keypair;

	/* Allocate Prime Number Modulus */
	key->p = crypto_bignum_allocate(l_bits);
	if (!key->p)
		goto err_alloc_keypair;

	/* Allocate Prime Number Modulus */
	key->q = crypto_bignum_allocate(n_bits);
	if (!key->q)
		goto err_alloc_keypair;

	/* Allocate Private key X */
	key->x = crypto_bignum_allocate(n_bits);
	if (!key->x)
		goto err_alloc_keypair;

	/* Allocate Public Key Y */
	key->y = crypto_bignum_allocate(l_bits);
	if (!key->y)
		goto err_alloc_keypair;

	return TEE_SUCCESS;

err_alloc_keypair:
	DSA_TRACE("Allocation error");

	crypto_bignum_free(key->g);
	crypto_bignum_free(key->p);
	crypto_bignum_free(key->q);
	crypto_bignum_free(key->x);

	return TEE_ERROR_OUT_OF_MEMORY;
}

/*
 * Allocate a DSA Public Key
 *
 * @key        Public Key
 * @l_bits     L bits size (prime p size)
 * @n_bits     N bits size (subprime q size)
 */
static TEE_Result do_allocate_publickey(struct dsa_public_key *key,
					size_t l_bits, size_t n_bits)
{
	DSA_TRACE("DSA Allocate Public of L=%zu bits and N=%zu bits", l_bits,
		  n_bits);

	/* Initialize the key fields to NULL */
	memset(key, 0, sizeof(*key));

	/* Allocate Generator Scalar */
	key->g = crypto_bignum_allocate(l_bits);
	if (!key->g)
		goto err_alloc_pubkey;

	/* Allocate Prime Number Modulus */
	key->p = crypto_bignum_allocate(l_bits);
	if (!key->p)
		goto err_alloc_pubkey;

	/* Allocate Prime Number Modulus */
	key->q = crypto_bignum_allocate(n_bits);
	if (!key->q)
		goto err_alloc_pubkey;

	/* Allocate Public Key Y */
	key->y = crypto_bignum_allocate(l_bits);
	if (!key->y)
		goto err_alloc_pubkey;

	return TEE_SUCCESS;

err_alloc_pubkey:
	DSA_TRACE("Allocation error");

	crypto_bignum_free(key->g);
	crypto_bignum_free(key->p);
	crypto_bignum_free(key->q);

	return TEE_ERROR_OUT_OF_MEMORY;
}

/*
 * Generates an DSA keypair
 * Keypair @key contains the input primes p, g and generator g values
 * The function calculates private x and public y.
 *
 * @key        [in/out] Keypair
 * @l_bits     L bits size (prime p size)
 * @n_bits     N bits size (subprime q size)
 */
static TEE_Result do_gen_keypair(struct dsa_keypair *key, size_t l_bits,
				 size_t n_bits)
{
	TEE_Result ret = TEE_ERROR_GENERIC;
	enum caam_status retstatus = CAAM_FAILURE;
	struct caam_dsa_keypair caam_dsa_key = {};
	struct caam_jobctx jobctx = {};
	uint32_t *desc = NULL;
	uint32_t desclen = 0;
	size_t l_bytes = l_bits / 8;
	size_t n_bytes = n_bits / 8;

#ifdef CFG_CAAM_64BIT
#define MAX_DESC_KEY_GEN 14
#else
#define MAX_DESC_KEY_GEN 9
#endif
	DSA_TRACE("Generate Key - Private (%zu bits) and Public (%zu bits)",
		  n_bits, l_bits);

	/* Allocate the job used to prepare the operation */
	desc = caam_calloc_desc(MAX_DESC_KEY_GEN);
	if (!desc) {
		ret = TEE_ERROR_OUT_OF_MEMORY;
		goto exit_gen_keypair;
	}

	/* Allocate Private Key to be generated */
	retstatus = caam_calloc_align_buf(&caam_dsa_key.x, n_bytes);
	if (retstatus != CAAM_NO_ERROR) {
		ret = TEE_ERROR_OUT_OF_MEMORY;
		goto exit_gen_keypair;
	}
	cache_operation(TEE_CACHEFLUSH, caam_dsa_key.x.data,
			caam_dsa_key.x.length);

	/* Allocate Public Key to be generated */
	retstatus = caam_calloc_align_buf(&caam_dsa_key.y, l_bytes);
	if (retstatus != CAAM_NO_ERROR) {
		ret = TEE_ERROR_OUT_OF_MEMORY;
		goto exit_gen_keypair;
	}
	cache_operation(TEE_CACHEFLUSH, caam_dsa_key.y.data,
			caam_dsa_key.y.length);

	/* Generator and Prime */
	ret = get_keypair_domain_params(&caam_dsa_key, key, l_bytes, n_bytes);
	if (ret != TEE_SUCCESS)
		goto exit_gen_keypair;

	/*
	 * Build the descriptor using the PDB Public Key generation
	 * block (PD=0)
	 */
	caam_desc_init(desc);
	caam_desc_add_word(desc, DESC_HEADER(0));
	caam_desc_add_word(desc, PDB_DL_KEY_L_SIZE(l_bytes) |
					 PDB_DL_KEY_N_SIZE(n_bytes));
	caam_desc_add_ptr(desc, caam_dsa_key.p.paddr);
	caam_desc_add_ptr(desc, caam_dsa_key.q.paddr);
	caam_desc_add_ptr(desc, caam_dsa_key.g.paddr);
	caam_desc_add_ptr(desc, caam_dsa_key.x.paddr);
	caam_desc_add_ptr(desc, caam_dsa_key.y.paddr);
	caam_desc_add_word(desc, PK_KEYPAIR_GEN(DL));

	desclen = caam_desc_get_len(desc);
	caam_desc_update_hdr(desc, DESC_HEADER_IDX(desclen, desclen - 1));

	DSA_DUMPDESC(desc);

	jobctx.desc = desc;
	retstatus = caam_jr_enqueue(&jobctx, NULL);

	if (retstatus == CAAM_NO_ERROR) {
		cache_operation(TEE_CACHEINVALIDATE, caam_dsa_key.x.data,
				caam_dsa_key.x.length);
		cache_operation(TEE_CACHEINVALIDATE, caam_dsa_key.y.data,
				caam_dsa_key.y.length);

		/* Copy Private and Public keypair */
		ret = crypto_bignum_bin2bn(caam_dsa_key.x.data,
					   caam_dsa_key.x.length, key->x);
		if (ret != TEE_SUCCESS)
			goto exit_gen_keypair;

		ret = crypto_bignum_bin2bn(caam_dsa_key.y.data,
					   caam_dsa_key.y.length, key->y);
		if (ret != TEE_SUCCESS)
			goto exit_gen_keypair;

		DSA_DUMPBUF("X", caam_dsa_key.x.data, caam_dsa_key.x.length);
		DSA_DUMPBUF("Y", caam_dsa_key.y.data, caam_dsa_key.y.length);

		ret = TEE_SUCCESS;
	} else {
		DSA_TRACE("CAAM Status 0x%08" PRIx32, jobctx.status);
		ret = job_status_to_tee_result(jobctx.status);
	}

exit_gen_keypair:
	caam_free_desc(&desc);
	do_keypair_free(&caam_dsa_key);

	return ret;
}

/*
 * Signature of DSA message
 * Note the message to sign is already hashed
 *
 * @sdata    [in/out] DSA data to sign / Signature
 * @l_bytes  L bytes size (prime p size)
 * @n_bytes  N bytes size (subprime q size)
 */
static TEE_Result do_sign(struct drvcrypt_sign_data *sdata, size_t l_bytes,
			  size_t n_bytes)
{
	TEE_Result ret = TEE_ERROR_GENERIC;
	enum caam_status retstatus = CAAM_FAILURE;
	struct dsa_keypair *inkey = sdata->key;
	struct caam_dsa_keypair dsakey = {};
	struct caam_jobctx jobctx = {};
	uint32_t *desc = NULL;
	uint32_t desclen = 0;
	struct caamdmaobj msg = {};
	size_t sign_len = 0;
	struct caamdmaobj sign_c = {};
	struct caamdmaobj sign_d = {};
	uint32_t pdb_sgt_flags = 0;

#ifdef CFG_CAAM_64BIT
#define MAX_DESC_SIGN 19
#else
#define MAX_DESC_SIGN 12
#endif

	DSA_TRACE("DSA Signature");

	/* Allocate the job descriptor */
	desc = caam_calloc_desc(MAX_DESC_SIGN);
	if (!desc) {
		ret = TEE_ERROR_OUT_OF_MEMORY;
		goto exit_sign;
	}

	/* Convert the private key to a local key */
	retstatus = do_keypriv_conv(&dsakey, inkey, l_bytes, n_bytes);
	if (retstatus != CAAM_NO_ERROR) {
		ret = TEE_ERROR_OUT_OF_MEMORY;
		goto exit_sign;
	}

	/* Prepare the input message CAAM Descriptor entry */
	ret = caam_dmaobj_init_input(&msg, sdata->message.data,
				     sdata->message.length);
	if (ret)
		goto exit_sign;

	if (msg.sgtbuf.sgt_type)
		pdb_sgt_flags |= PDB_SGT_PKSIGN_MSG;

	caam_dmaobj_cache_push(&msg);

	DSA_DUMPBUF("Message", sdata->message.data, sdata->message.length);

	/*
	 * ReAllocate the signature result buffer with a maximum size
	 * of the roundup to 16 bytes of the secure size in bytes if
	 * the signature buffer is not aligned or too short.
	 *
	 *  - 1st Part: size_sec
	 *  - 2nd Part: size_sec roundup to 16 bytes
	 */
	sign_len = ROUNDUP(sdata->size_sec, 16) + sdata->size_sec;

	ret = caam_dmaobj_init_output(&sign_c, sdata->signature.data,
				      sdata->signature.length, sign_len);
	if (ret)
		goto exit_sign;

	if (sign_c.sgtbuf.sgt_type)
		pdb_sgt_flags |= PDB_SGT_PKSIGN_SIGN_C;

	/* Prepare the 2nd Part of the signature. Derive from sign_c */
	ret = caam_dmaobj_derive(&sign_d, &sign_c, sdata->size_sec,
				 ROUNDUP(sdata->size_sec, 16));
	if (ret)
		goto exit_sign;

	if (sign_d.sgtbuf.sgt_type)
		pdb_sgt_flags |= PDB_SGT_PKSIGN_SIGN_D;

	caam_dmaobj_cache_push(&sign_c);
	caam_dmaobj_cache_push(&sign_d);

	/*
	 * Build the descriptor using Predifined ECC curve
	 */
	caam_desc_init(desc);
	caam_desc_add_word(desc, DESC_HEADER(0));
	caam_desc_add_word(desc, PDB_DSA_SIGN_N(n_bytes) |
					 PDB_DSA_SIGN_L(l_bytes) |
					 pdb_sgt_flags);
	/* Prime number */
	caam_desc_add_ptr(desc, dsakey.p.paddr);
	/* Prime Modulus */
	caam_desc_add_ptr(desc, dsakey.q.paddr);
	/* Generator */
	caam_desc_add_ptr(desc, dsakey.g.paddr);
	/* Secret key */
	caam_desc_add_ptr(desc, dsakey.x.paddr);
	/* Input message */
	caam_desc_add_ptr(desc, msg.sgtbuf.paddr);
	/* Signature 1st part */
	caam_desc_add_ptr(desc, sign_c.sgtbuf.paddr);
	/* Signature 2nd part */
	caam_desc_add_ptr(desc, sign_d.sgtbuf.paddr);
	/* Message length */
	caam_desc_add_word(desc, sdata->message.length);

	caam_desc_add_word(desc, DSA_SIGN(DL));

	desclen = caam_desc_get_len(desc);
	caam_desc_update_hdr(desc, DESC_HEADER_IDX(desclen, desclen - 1));

	ECC_DUMPDESC(desc);

	jobctx.desc = desc;

	retstatus = caam_jr_enqueue(&jobctx, NULL);
	if (retstatus == CAAM_NO_ERROR) {
		/* Limit the copy to 2 * sdata->size_sec */
		sign_c.orig.length = 2 * sdata->size_sec;
		caam_dmaobj_copy_to_orig(&sign_c);

		sdata->signature.length = sign_c.orig.length;

		DSA_DUMPBUF("Signature", sdata->signature.data,
			    sdata->signature.length);

		ret = TEE_SUCCESS;
	} else {
		DSA_TRACE("CAAM Status 0x%08" PRIx32, jobctx.status);
		ret = job_status_to_tee_result(jobctx.status);
	}

exit_sign:
	caam_free_desc(&desc);
	do_keypair_free(&dsakey);
	caam_dmaobj_free(&msg);
	caam_dmaobj_free(&sign_c);
	caam_dmaobj_free(&sign_d);

	return ret;
}

/*
 * Verification of the Signature of DSA message
 * Note the message is already hashed
 *
 * @sdata   [in/out] DSA Signature to verify
 * @l_bytes  L bytes size (prime p size)
 * @n_bytes  N bytes size (subprime q size)
 */
static TEE_Result do_verify(struct drvcrypt_sign_data *sdata, size_t l_bytes,
			    size_t n_bytes)
{
	TEE_Result ret = TEE_ERROR_GENERIC;
	enum caam_status retstatus = CAAM_FAILURE;
	struct dsa_public_key *inkey = sdata->key;
	struct caam_dsa_keypair dsakey = {};
	struct caambuf tmp = {};
	struct caam_jobctx jobctx = {};
	uint32_t *desc = NULL;
	uint32_t desclen = 0;
	struct caamdmaobj msg = {};
	struct caamdmaobj sign_c = {};
	struct caamdmaobj sign_d = {};
	uint32_t pdb_sgt_flags = 0;

#ifdef CFG_CAAM_64BIT
#define MAX_DESC_VERIFY 21
#else
#define MAX_DESC_VERIFY 13
#endif

	DSA_TRACE("DSA Verify");

	/* Allocate the job descriptor */
	desc = caam_calloc_desc(MAX_DESC_VERIFY);
	if (!desc) {
		ret = TEE_ERROR_OUT_OF_MEMORY;
		goto exit_verify;
	}

	/* Convert the Public key to local key */
	retstatus = do_keypub_conv(&dsakey, inkey, l_bytes, n_bytes);
	if (retstatus != CAAM_NO_ERROR) {
		ret = TEE_ERROR_OUT_OF_MEMORY;
		goto exit_verify;
	}

	/* Prepare the input message CAAM Descriptor entry */
	ret = caam_dmaobj_init_input(&msg, sdata->message.data,
				     sdata->message.length);
	if (ret)
		goto exit_verify;

	if (msg.sgtbuf.sgt_type)
		pdb_sgt_flags |= PDB_SGT_PKVERIF_MSG;

	caam_dmaobj_cache_push(&msg);

	/*
	 * Prepare the 1st Part of the signature
	 * Handle the full signature in case signature buffer needs to
	 * be reallocated.
	 */
	ret = caam_dmaobj_init_input(&sign_c, sdata->signature.data,
				     sdata->signature.length);
	if (ret)
		goto exit_verify;

	if (sign_c.sgtbuf.sgt_type)
		pdb_sgt_flags |= PDB_SGT_PKVERIF_SIGN_C;

	/* Prepare the 2nd Part of the signature, derive from sign_c */
	ret = caam_dmaobj_derive(&sign_d, &sign_c, sdata->size_sec,
				 sdata->size_sec);
	if (ret)
		goto exit_verify;

	if (sign_d.sgtbuf.sgt_type)
		pdb_sgt_flags |= PDB_SGT_PKVERIF_SIGN_D;

	caam_dmaobj_cache_push(&sign_c);
	caam_dmaobj_cache_push(&sign_d);

	/* Allocate a Temporary buffer used by the CAAM */
	retstatus = caam_alloc_align_buf(&tmp, l_bytes);
	if (retstatus != CAAM_NO_ERROR) {
		ret = TEE_ERROR_OUT_OF_MEMORY;
		goto exit_verify;
	}

	/*
	 * Build the descriptor using Predifined ECC curve
	 */
	caam_desc_init(desc);
	caam_desc_add_word(desc, DESC_HEADER(0));
	caam_desc_add_word(desc, PDB_DSA_VERIF_N(n_bytes) |
					 PDB_DSA_VERIF_L(l_bytes) |
					 pdb_sgt_flags);
	/* Prime number */
	caam_desc_add_ptr(desc, dsakey.p.paddr);
	/* Prime Modulus */
	caam_desc_add_ptr(desc, dsakey.q.paddr);
	/* Generator */
	caam_desc_add_ptr(desc, dsakey.g.paddr);
	/* Public key */
	caam_desc_add_ptr(desc, dsakey.y.paddr);
	/* Input message */
	caam_desc_add_ptr(desc, msg.sgtbuf.paddr);
	/* Signature 1st part */
	caam_desc_add_ptr(desc, sign_c.sgtbuf.paddr);
	/* Signature 2nd part */
	caam_desc_add_ptr(desc, sign_d.sgtbuf.paddr);
	/* Temporary buffer */
	caam_desc_add_ptr(desc, tmp.paddr);
	/* Message length */
	caam_desc_add_word(desc, sdata->message.length);

	caam_desc_add_word(desc, DSA_VERIFY(DL));
	desclen = caam_desc_get_len(desc);
	caam_desc_update_hdr(desc, DESC_HEADER_IDX(desclen, desclen - 1));

	DSA_DUMPDESC(desc);

	jobctx.desc = desc;

	cache_operation(TEE_CACHEFLUSH, tmp.data, tmp.length);
	retstatus = caam_jr_enqueue(&jobctx, NULL);

	if (retstatus == CAAM_JOB_STATUS && !jobctx.status) {
		DSA_TRACE("DSA Verify Status 0x%08" PRIx32, jobctx.status);
		ret = TEE_ERROR_SIGNATURE_INVALID;
	} else if (retstatus != CAAM_NO_ERROR) {
		DSA_TRACE("CAAM Status 0x%08" PRIx32, jobctx.status);
		ret = job_status_to_tee_result(jobctx.status);
	} else {
		ret = TEE_SUCCESS;
	}

exit_verify:
	caam_free_desc(&desc);
	do_keypair_free(&dsakey);
	caam_free_buf(&tmp);
	caam_dmaobj_free(&msg);
	caam_dmaobj_free(&sign_c);
	caam_dmaobj_free(&sign_d);

	return ret;
}

/*
 * Registration of the DSA Driver
 */
static struct drvcrypt_dsa driver_dsa = {
	.alloc_keypair = do_allocate_keypair,
	.alloc_publickey = do_allocate_publickey,
	.gen_keypair = do_gen_keypair,
	.sign = do_sign,
	.verify = do_verify,
};

enum caam_status caam_dsa_init(struct caam_jrcfg *caam_jrcfg)
{
	enum caam_status retstatus = CAAM_FAILURE;
	vaddr_t jr_base = caam_jrcfg->base + caam_jrcfg->offset;

	if (caam_hal_ctrl_pknum(jr_base))
		if (drvcrypt_register_dsa(&driver_dsa) == TEE_SUCCESS)
			retstatus = CAAM_NO_ERROR;

	return retstatus;
}
