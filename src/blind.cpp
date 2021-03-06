#include "blind.h"

#include "hash.h"
#include "primitives/transaction.h"
#include "random.h"
#include "util.h"

#include <secp256k1.h>

static secp256k1_context_t* secp256k1_context = NULL;

void ECC_Blinding_Start() {
    assert(secp256k1_context == NULL);

    secp256k1_context_t *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY | SECP256K1_CONTEXT_COMMIT | SECP256K1_CONTEXT_RANGEPROOF);
    assert(ctx != NULL);

    secp256k1_context = ctx;
}

void ECC_Blinding_Stop() {
    secp256k1_context_t *ctx = secp256k1_context;
    secp256k1_context = NULL;

    if (ctx) {
        secp256k1_context_destroy(ctx);
    }
}

const secp256k1_context_t* ECC_Blinding_Context() {
    return secp256k1_context;
}

int UnblindOutput(const CKey &key, const CTxOut& txout, CAmount& amount_out, std::vector<unsigned char>& blinding_factor_out)
{
    if (txout.nValue.IsAmount()) {
        amount_out = txout.nValue.GetAmount();
        blinding_factor_out.resize(0);
        return -1;
    }
    CPubKey ephemeral_key(txout.nValue.vchNonceCommitment);
    if (!ephemeral_key.IsValid()) {
        return 0;
    }
    CPubKey nonce_key = key.ECDH(ephemeral_key);
    unsigned char nonce[32];
    CHash256().Write(nonce_key.begin(), nonce_key.size()).Finalize(nonce);
    unsigned char msg[4096];
    int msg_size;
    uint64_t min_value, max_value, amount;
    blinding_factor_out.resize(32);
    int res = secp256k1_rangeproof_rewind(secp256k1_context, &blinding_factor_out[0], &amount, msg, &msg_size, nonce, &min_value, &max_value, &txout.nValue.vchCommitment[0], &txout.nValue.vchRangeproof[0], txout.nValue.vchRangeproof.size());
    if (!res || amount > (uint64_t)MAX_MONEY || !MoneyRange((CAmount)amount)) {
        amount_out = 0;
        blinding_factor_out.resize(0);
    } else
        amount_out = (CAmount)amount;
    return res ? 1 : 0;
}

void BlindOutputs(const std::vector<std::vector<unsigned char> >& input_blinding_factors, const std::vector<std::vector<unsigned char> >& output_blinding_factors, const std::vector<CPubKey>& output_pubkeys, CMutableTransaction& tx)
{
    assert(tx.vout.size() == output_blinding_factors.size());
    assert(tx.vout.size() == output_pubkeys.size());
    assert(tx.vin.size() == input_blinding_factors.size());

    std::vector<const unsigned char*> blindptrs;
    blindptrs.reserve(tx.vout.size() + tx.vin.size());

    int nBlindsIn = 0;
    for (size_t nIn = 0; nIn < tx.vin.size(); nIn++) {
        if (input_blinding_factors[nIn].size() != 0) {
            assert(input_blinding_factors[nIn].size() == 32);
            blindptrs.push_back(&input_blinding_factors[nIn][0]);
            nBlindsIn++;
        }
    }

    int nBlindsOut = 0;
    int nToBlind = 0;
    for (size_t nOut = 0; nOut < tx.vout.size(); nOut++) {
         assert((output_blinding_factors[nOut].size() != 0) == !tx.vout[nOut].nValue.IsAmount());
         if (output_blinding_factors[nOut].size() != 0) {
             assert(output_blinding_factors[nOut].size() == 32);
             blindptrs.push_back(&output_blinding_factors[nOut][0]);
             nBlindsOut++;
         } else {
             if (output_pubkeys[nOut].IsValid()) {
                 nToBlind++;
             }
         }
    }

    if (nBlindsIn != 0) {
        assert((nBlindsOut + nToBlind) != 0);
    }

    int nBlinded = 0;
    unsigned char blind[nToBlind][32];

    for (size_t nOut = 0; nOut < tx.vout.size(); nOut++) {
        if (tx.vout[nOut].nValue.IsAmount() && output_pubkeys[nOut].IsValid()) {
            assert(output_pubkeys[nOut].IsValid());
            if (nBlinded + 1 == nToBlind) {
                // Last to-be-blinded value: compute from all other blinding factors.
                assert(secp256k1_pedersen_blind_sum(ECC_Blinding_Context(), &blind[nBlinded][0], &blindptrs[0], nBlindsOut + nBlindsIn, nBlindsIn));
                blindptrs.push_back(&blind[nBlinded++][0]);
            } else {
                GetRandBytes(&blind[nBlinded][0], 32);
                blindptrs.push_back(&blind[nBlinded++][0]);
            }
            nBlindsOut++;
            // Create blinded value
            CTxOutValue& value = tx.vout[nOut].nValue;
            CAmount amount = value.GetAmount();
            assert(secp256k1_pedersen_commit(ECC_Blinding_Context(), &value.vchCommitment[0], (unsigned char*)blindptrs.back(), amount));
            // Generate ephemeral key for ECDH nonce generation
            CKey ephemeral_key;
            ephemeral_key.MakeNewKey(true);
            CPubKey ephemeral_pubkey = ephemeral_key.GetPubKey();
            value.vchNonceCommitment.resize(33);
            memcpy(&value.vchNonceCommitment[0], &ephemeral_pubkey[0], 33);
            // Generate nonce
            CPubKey nonce_key = ephemeral_key.ECDH(output_pubkeys[nOut]);
            unsigned char nonce[32];
            CHash256().Write(nonce_key.begin(), nonce_key.size()).Finalize(nonce);
            // Create range proof
            int nRangeProofLen = 5134;
            // TODO: smarter min_value selection
            value.vchRangeproof.resize(nRangeProofLen);
            int res = secp256k1_rangeproof_sign(ECC_Blinding_Context(), &value.vchRangeproof[0], &nRangeProofLen, 0, &value.vchCommitment[0], blindptrs.back(), nonce, std::min(std::max((int)GetArg("-ct_exponent", 0), -1),18), std::min(std::max((int)GetArg("-ct_bits", 32), 1), 51), amount);
            value.vchRangeproof.resize(nRangeProofLen);
            // TODO: do something smarter here
            assert(res);
        }
    }
}
