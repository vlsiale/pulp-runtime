#include "indcpa.h"
#include "ntt.h"
#include "params.h"
#include "poly.h"
#include "polyvec.h"
#include "randombytes.h"
#include "symmetric.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*************************************************
* Name:        pack_pk
*
* Description: Serialize the public key as concatenation of the
*              serialized vector of polynomials pk
*              and the public seed used to generate the matrix A.
*
* Arguments:   uint8_t *r: pointer to the output serialized public key
*              polyvec *pk: pointer to the input public-key polyvec
*              const uint8_t *seed: pointer to the input public seed
**************************************************/
static void pack_pk(uint8_t r[KYBER_INDCPA_PUBLICKEYBYTES],
                    polyvec *pk,
                    const uint8_t seed[KYBER_SYMBYTES]) {
    size_t i;
    PQCLEAN_KYBER768_CLEAN_polyvec_tobytes(r, pk);
    for (i = 0; i < KYBER_SYMBYTES; i++) {
        r[i + KYBER_POLYVECBYTES] = seed[i];
    }
}

/*************************************************
* Name:        unpack_pk
*
* Description: De-serialize public key from a byte array;
*              approximate inverse of pack_pk
*
* Arguments:   - polyvec *pk: pointer to output public-key polynomial vector
*              - uint8_t *seed: pointer to output seed to generate matrix A
*              - const uint8_t *packedpk: pointer to input serialized public key
**************************************************/
static void unpack_pk(polyvec *pk,
                      uint8_t seed[KYBER_SYMBYTES],
                      const uint8_t packedpk[KYBER_INDCPA_PUBLICKEYBYTES]) {
    size_t i;
    PQCLEAN_KYBER768_CLEAN_polyvec_frombytes(pk, packedpk);
    for (i = 0; i < KYBER_SYMBYTES; i++) {
        seed[i] = packedpk[i + KYBER_POLYVECBYTES];
    }
}

/*************************************************
* Name:        pack_sk
*
* Description: Serialize the secret key
*
* Arguments:   - uint8_t *r: pointer to output serialized secret key
*              - polyvec *sk: pointer to input vector of polynomials (secret key)
**************************************************/
static void pack_sk(uint8_t r[KYBER_INDCPA_SECRETKEYBYTES], polyvec *sk) {
    PQCLEAN_KYBER768_CLEAN_polyvec_tobytes(r, sk);
}

/*************************************************
* Name:        unpack_sk
*
* Description: De-serialize the secret key; inverse of pack_sk
*
* Arguments:   - polyvec *sk: pointer to output vector of polynomials (secret key)
*              - const uint8_t *packedsk: pointer to input serialized secret key
**************************************************/
static void unpack_sk(polyvec *sk, const uint8_t packedsk[KYBER_INDCPA_SECRETKEYBYTES]) {
    PQCLEAN_KYBER768_CLEAN_polyvec_frombytes(sk, packedsk);
}

/*************************************************
* Name:        pack_ciphertext
*
* Description: Serialize the ciphertext as concatenation of the
*              compressed and serialized vector of polynomials b
*              and the compressed and serialized polynomial v
*
* Arguments:   uint8_t *r: pointer to the output serialized ciphertext
*              poly *pk: pointer to the input vector of polynomials b
*              poly *v: pointer to the input polynomial v
**************************************************/
static void pack_ciphertext(uint8_t r[KYBER_INDCPA_BYTES], polyvec *b, poly *v) {
    PQCLEAN_KYBER768_CLEAN_polyvec_compress(r, b);
    PQCLEAN_KYBER768_CLEAN_poly_compress(r + KYBER_POLYVECCOMPRESSEDBYTES, v);
}

/*************************************************
* Name:        unpack_ciphertext
*
* Description: De-serialize and decompress ciphertext from a byte array;
*              approximate inverse of pack_ciphertext
*
* Arguments:   - polyvec *b: pointer to the output vector of polynomials b
*              - poly *v: pointer to the output polynomial v
*              - const uint8_t *c: pointer to the input serialized ciphertext
**************************************************/
static void unpack_ciphertext(polyvec *b, poly *v, const uint8_t c[KYBER_INDCPA_BYTES]) {
    PQCLEAN_KYBER768_CLEAN_polyvec_decompress(b, c);
    PQCLEAN_KYBER768_CLEAN_poly_decompress(v, c + KYBER_POLYVECCOMPRESSEDBYTES);
}

/*************************************************
* Name:        rej_uniform
*
* Description: Run rejection sampling on uniform random bytes to generate
*              uniform random integers mod q
*
* Arguments:   - int16_t *r: pointer to output buffer
*              - unsigned int len: requested number of 16-bit integers (uniform mod q)
*              - const uint8_t *buf: pointer to input buffer (assumed to be uniformly random bytes)
*              - unsigned int buflen: length of input buffer in bytes
*
* Returns number of sampled 16-bit integers (at most len)
**************************************************/
static unsigned int rej_uniform(int16_t *r,
                                unsigned int len,
                                const uint8_t *buf,
                                unsigned int buflen) {
    unsigned int ctr, pos;
    uint16_t val0, val1;

    ctr = pos = 0;
    while (ctr < len && pos + 3 <= buflen) {
        val0 = ((buf[pos + 0] >> 0) | ((uint16_t)buf[pos + 1] << 8)) & 0xFFF;
        val1 = ((buf[pos + 1] >> 4) | ((uint16_t)buf[pos + 2] << 4)) & 0xFFF;
        pos += 3;

        if (val0 < KYBER_Q) {
            r[ctr++] = val0;
        }
        if (ctr < len && val1 < KYBER_Q) {
            r[ctr++] = val1;
        }
    }

    return ctr;
}

#define gen_a(A,B)  PQCLEAN_KYBER768_CLEAN_gen_matrix(A,B,0)
#define gen_at(A,B) PQCLEAN_KYBER768_CLEAN_gen_matrix(A,B,1)

/*************************************************
* Name:        PQCLEAN_KYBER768_CLEAN_gen_matrix
*
* Description: Deterministically generate matrix A (or the transpose of A)
*              from a seed. Entries of the matrix are polynomials that look
*              uniformly random. Performs rejection sampling on output of
*              a XOF
*
* Arguments:   - polyvec *a: pointer to ouptput matrix A
*              - const uint8_t *seed: pointer to input seed
*              - int transposed: boolean deciding whether A or A^T is generated
**************************************************/
#define GEN_MATRIX_NBLOCKS ((12*KYBER_N/8*(1 << 12)/KYBER_Q + XOF_BLOCKBYTES)/XOF_BLOCKBYTES)
// Not static for benchmarking
void PQCLEAN_KYBER768_CLEAN_gen_matrix(polyvec *a, const uint8_t seed[KYBER_SYMBYTES], int transposed) {
    unsigned int ctr, i, j, k;
    unsigned int buflen, off;
    uint8_t buf[GEN_MATRIX_NBLOCKS * XOF_BLOCKBYTES + 2];
    xof_state state;

    for (i = 0; i < KYBER_K; i++) {
        for (j = 0; j < KYBER_K; j++) {
            if (transposed) {
                xof_absorb(&state, seed, (uint8_t)i, (uint8_t)j);
            } else {
                xof_absorb(&state, seed, (uint8_t)j, (uint8_t)i);
            }

            xof_squeezeblocks(buf, GEN_MATRIX_NBLOCKS, &state);
            buflen = GEN_MATRIX_NBLOCKS * XOF_BLOCKBYTES;
            ctr = rej_uniform(a[i].vec[j].coeffs, KYBER_N, buf, buflen);

            while (ctr < KYBER_N) {
                off = buflen % 3;
                for (k = 0; k < off; k++) {
                    buf[k] = buf[buflen - off + k];
                }
                xof_squeezeblocks(buf + off, 1, &state);
                buflen = off + XOF_BLOCKBYTES;
                ctr += rej_uniform(a[i].vec[j].coeffs + ctr, KYBER_N - ctr, buf, buflen);
            }
            xof_ctx_release(&state);
        }
    }
}

/*************************************************
* Name:        PQCLEAN_KYBER768_CLEAN_indcpa_keypair
*
* Description: Generates public and private key for the CPA-secure
*              public-key encryption scheme underlying Kyber
*
* Arguments:   - uint8_t *pk: pointer to output public key
*                             (of length KYBER_INDCPA_PUBLICKEYBYTES bytes)
*              - uint8_t *sk: pointer to output private key
                              (of length KYBER_INDCPA_SECRETKEYBYTES bytes)
**************************************************/
void PQCLEAN_KYBER768_CLEAN_indcpa_keypair(uint8_t pk[KYBER_INDCPA_PUBLICKEYBYTES],
        uint8_t sk[KYBER_INDCPA_SECRETKEYBYTES]) {
    unsigned int i;
    uint8_t buf[2 * KYBER_SYMBYTES];
    const uint8_t *publicseed = buf;
    const uint8_t *noiseseed = buf + KYBER_SYMBYTES;
    uint8_t nonce = 0;
    polyvec a[KYBER_K], e, pkpv, skpv;
    uint8_t A;

    randombytes(buf, KYBER_SYMBYTES);
    /*printf("\nRANDOM BUF:\n");
    for (int j = 0; j < 64; j++) {
        A = buf[j];
        printf("%02X-", A);
    }*/
    hash_g(buf, buf, KYBER_SYMBYTES);

    gen_a(a, publicseed);

    for (i = 0; i < KYBER_K; i++) {
        PQCLEAN_KYBER768_CLEAN_poly_getnoise_eta1(&skpv.vec[i], noiseseed, nonce++);
    }
    for (i = 0; i < KYBER_K; i++) {
        PQCLEAN_KYBER768_CLEAN_poly_getnoise_eta1(&e.vec[i], noiseseed, nonce++);
    }

    
    PQCLEAN_KYBER768_CLEAN_polyvec_ntt(&skpv);    
    PQCLEAN_KYBER768_CLEAN_polyvec_ntt(&e);

    // matrix-vector multiplication
    for (i = 0; i < KYBER_K; i++) {
        PQCLEAN_KYBER768_CLEAN_polyvec_basemul_acc_montgomery(&pkpv.vec[i], &a[i], &skpv);
        PQCLEAN_KYBER768_CLEAN_poly_tomont(&pkpv.vec[i]);
    }

    PQCLEAN_KYBER768_CLEAN_polyvec_add(&pkpv, &pkpv, &e);
    PQCLEAN_KYBER768_CLEAN_polyvec_reduce(&pkpv);

    pack_sk(sk, &skpv);
    pack_pk(pk, &pkpv, publicseed);
}

/*************************************************
* Name:        PQCLEAN_KYBER768_CLEAN_indcpa_enc
*
* Description: Encryption function of the CPA-secure
*              public-key encryption scheme underlying Kyber.
*
* Arguments:   - uint8_t *c: pointer to output ciphertext
*                            (of length KYBER_INDCPA_BYTES bytes)
*              - const uint8_t *m: pointer to input message
*                                  (of length KYBER_INDCPA_MSGBYTES bytes)
*              - const uint8_t *pk: pointer to input public key
*                                   (of length KYBER_INDCPA_PUBLICKEYBYTES)
*              - const uint8_t *coins: pointer to input random coins used as seed
*                                      (of length KYBER_SYMBYTES) to deterministically
*                                      generate all randomness
**************************************************/
void PQCLEAN_KYBER768_CLEAN_indcpa_enc(uint8_t c[KYBER_INDCPA_BYTES],
                                       const uint8_t m[KYBER_INDCPA_MSGBYTES],
                                       const uint8_t pk[KYBER_INDCPA_PUBLICKEYBYTES],
                                       const uint8_t coins[KYBER_SYMBYTES]) {
    unsigned int i;
    uint8_t seed[KYBER_SYMBYTES];
    uint8_t nonce = 0;
    polyvec sp, pkpv, ep, at[KYBER_K], b;
    poly v, k, epp;
    
    poly vector;
    uint32_t Din_v[128], Dout_v[128];
	  uint32_t concatenated1, concatenated2;
    uint16_t A;

    unpack_pk(&pkpv, seed, pk);
    PQCLEAN_KYBER768_CLEAN_poly_frommsg(&k, m);
    gen_at(at, seed);

    /*printf("\nCOINS:\n");
    for (int j = 0; j < 32; j++) {
        A = coins[j];
        printf("%02X-", A);
    }*/
    for (i = 0; i < KYBER_K; i++) {
        PQCLEAN_KYBER768_CLEAN_poly_getnoise_eta1(sp.vec + i, coins, nonce++);
    }
    
    /*printf("\nsp after getnoise:\n");
    for (i = 0; i < KYBER_K; i++) {
        for (int j = 0; j < KYBER_N; j++) {
            printf("%04X-", sp.vec[i].coeffs[j]);
        }
        printf("\n------------------------------\n");
    }*/
    
    
    for (i = 0; i < KYBER_K; i++) {
        PQCLEAN_KYBER768_CLEAN_poly_getnoise_eta2(ep.vec + i, coins, nonce++);
    }
    PQCLEAN_KYBER768_CLEAN_poly_getnoise_eta2(&epp, coins, nonce++);

    /*printf("\nsp before ntt:\n");
    for (i = 0; i < KYBER_K; i++) {
        for (int j = 0; j < KYBER_N; j++) {
            printf("%04X-", sp.vec[i].coeffs[j]);
        }
        printf("\n------------------------------\n");
    }*/
    
    PQCLEAN_KYBER768_CLEAN_polyvec_ntt(&sp);
    
    /*printf("\n\nsp after ntt:\n");
    for (i = 0; i < KYBER_K; i++) {
        for (int j = 0; j < KYBER_N; j++) {
            printf("%04X-", sp.vec[i].coeffs[j]);
        }
        printf("\n------------------------------\n");
    }*/

    // matrix-vector multiplication
    for (i = 0; i < KYBER_K; i++) {
        PQCLEAN_KYBER768_CLEAN_polyvec_basemul_acc_montgomery(&b.vec[i], &at[i], &sp);
    }

    PQCLEAN_KYBER768_CLEAN_polyvec_basemul_acc_montgomery(&v, &pkpv, &sp);
    /*printf("KEY-B before intt:\n");
    for (i = 0; i < KYBER_K; i++) {
        for (int j = 0; j < KYBER_N; j++) {
            A = b.vec[i].coeffs[j];
            printf("%04X-", A);
        }
    }*/
    PQCLEAN_KYBER768_CLEAN_polyvec_invntt_tomont(&b);
    /*printf("\n\nKEY-B after intt:\n");
    for (i = 0; i < KYBER_K; i++) {
        for (int j = 0; j < KYBER_N; j++) {
            A = b.vec[i].coeffs[j];
            printf("%04X-", A);
        }
    }*/
    
    

    vector = v;
        

    for (int k1 = 0; k1 <= 254; k1 += 4) {
        concatenated1 = 0;
        concatenated2 = 0;
        concatenated1 = ((uint32_t)vector.coeffs[k1] << 16) | ((uint32_t)vector.coeffs[k1 + 2] & 0xFFFF);
        concatenated2 = ((uint32_t)vector.coeffs[k1+1] << 16) | ((uint32_t)vector.coeffs[k1 + 3] & 0xFFFF);
        Din_v[k1 / 2] = concatenated1;
        Din_v[k1 / 2 + 1] = concatenated2;
    }
        
    /*printf("\npoly_INTT INPUT on 32bit *******************************\n");
    for (int j = 0; j < 128; j++) {
        printf("%08x-", Din_v[j]);
    }
    printf("\n");
        
     
    printf("\nINTT accelerator starts working!\n");*/
    KYBER_poly_intt(Din_v, Dout_v);
    /*printf("\nINTT accelerator ends working!\n");
   
    printf("\nOUTPUT from INVNTT - DOUT [%d]\n", i);
    for (int i = 0; i < 128; i++) {	 
      printf("%08x-",Dout_v[i]);
    }*/

    for (int k2 = 0; k2 < 128; k2++) {
      uint32_t value = Dout_v[k2];
      uint16_t msb = (value >> 16) & 0xFFFF;
      uint16_t lsb = value & 0xFFFF;

      // Assign the MSB and LSB to the corresponding elements in r
      v.coeffs[k2] = msb; // Assign MSB
      v.coeffs[k2 + 128] = lsb; // Assign LSB
    }
    
    /*printf("\nOUTPUT INVNTT from polyvec [%d]\n", i);
    for (int k3 = 0; k3 < 256; k3++) {
        printf("%04x-", v.coeffs[k3]);
    }*/
    
    
    //PQCLEAN_KYBER768_CLEAN_poly_invntt_tomont(&v);
    

    PQCLEAN_KYBER768_CLEAN_polyvec_add(&b, &b, &ep);
    PQCLEAN_KYBER768_CLEAN_poly_add(&v, &v, &epp);
    PQCLEAN_KYBER768_CLEAN_poly_add(&v, &v, &k);
    PQCLEAN_KYBER768_CLEAN_polyvec_reduce(&b);
    PQCLEAN_KYBER768_CLEAN_poly_reduce(&v);

    pack_ciphertext(c, &b, &v);
}

/*************************************************
* Name:        PQCLEAN_KYBER768_CLEAN_indcpa_dec
*
* Description: Decryption function of the CPA-secure
*              public-key encryption scheme underlying Kyber.
*
* Arguments:   - uint8_t *m: pointer to output decrypted message
*                            (of length KYBER_INDCPA_MSGBYTES)
*              - const uint8_t *c: pointer to input ciphertext
*                                  (of length KYBER_INDCPA_BYTES)
*              - const uint8_t *sk: pointer to input secret key
*                                   (of length KYBER_INDCPA_SECRETKEYBYTES)
**************************************************/
void PQCLEAN_KYBER768_CLEAN_indcpa_dec(uint8_t m[KYBER_INDCPA_MSGBYTES],
                                       const uint8_t c[KYBER_INDCPA_BYTES],
                                       const uint8_t sk[KYBER_INDCPA_SECRETKEYBYTES]) {
    polyvec b, skpv;
    poly v, mp;
    
    poly vector;
    uint32_t Din[128], Dout[128];
	  uint32_t concatenated1, concatenated2;
    uint16_t A;

    unpack_ciphertext(&b, &v, c);
    unpack_sk(&skpv, sk);
   
    PQCLEAN_KYBER768_CLEAN_polyvec_ntt(&b);
    PQCLEAN_KYBER768_CLEAN_polyvec_basemul_acc_montgomery(&mp, &skpv, &b);
    
    
    //PQCLEAN_KYBER768_CLEAN_poly_invntt_tomont(&mp);
    
    vector = mp;
        

    for (int k1 = 0; k1 <= 254; k1 += 4) {
        concatenated1 = 0;
        concatenated2 = 0;
        concatenated1 = ((uint32_t)vector.coeffs[k1] << 16) | ((uint32_t)vector.coeffs[k1 + 2] & 0xFFFF);
        concatenated2 = ((uint32_t)vector.coeffs[k1+1] << 16) | ((uint32_t)vector.coeffs[k1 + 3] & 0xFFFF);
        Din[k1 / 2] = concatenated1;
        Din[k1 / 2 + 1] = concatenated2;
    }
        
     
    //printf("\nINTT accelerator starts working!\n");
    KYBER_poly_intt(Din, Dout);
    /*printf("\nINTT accelerator ends working!\n");
   
    printf("\nOUTPUT from INVNTT - DOUT\n");
    for (int i = 0; i < 128; i++) {	 
      printf("%08x-",Dout[i]);
    }*/

    for (int k2 = 0; k2 < 128; k2++) {
      uint32_t value = Dout[k2];
      uint16_t msb = (value >> 16) & 0xFFFF;
      uint16_t lsb = value & 0xFFFF;

      // Assign the MSB and LSB to the corresponding elements in r
      mp.coeffs[k2] = msb; // Assign MSB
      mp.coeffs[k2 + 128] = lsb; // Assign LSB
    }
    
    /*printf("\nOUTPUT INVNTT from polyvec\n");
    for (int k3 = 0; k3 < 256; k3++) {
        printf("%04x-", v.coeffs[k3]);
    }*/

    PQCLEAN_KYBER768_CLEAN_poly_sub(&mp, &v, &mp);
    PQCLEAN_KYBER768_CLEAN_poly_reduce(&mp);

    PQCLEAN_KYBER768_CLEAN_poly_tomsg(m, &mp);
}