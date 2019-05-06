#ifndef SIGMA_COINSPEND_H
#define SIGMA_COINSPEND_H

#include <sigma/coin.h>
#include <sigma/sigmaplus_proof.h>
#include <sigma/sigmaplus_prover.h>
#include <sigma/sigmaplus_verifier.h>
#include <sigma/spend_metadata.h>

using namespace secp_primitives;

namespace sigma {

class CoinSpend {
public:
    template<typename Stream>
    CoinSpend(const Params* p,  Stream& strm):
        params(p),
        denomination(CoinDenomination::SIGMA_1),
        sigmaProof(p) {
            strm >> * this;
        }


    CoinSpend(const Params* p,
              const PrivateCoin& coin,
              const std::vector<PublicCoin>& anonymity_set,
              const SpendMetaData& m);

    void updateMetaData(const PrivateCoin& coin, const SpendMetaData& m);

    const Scalar& getCoinSerialNumber();

    CoinDenomination getDenomination() const;

    int64_t getIntDenomination() const;

    void setVersion(unsigned int nVersion){
        version = nVersion;
    }

    int getVersion() const {
        return version;
    }

    uint256 getAccumulatorBlockHash() const {
        return accumulatorBlockHash;
    }

    bool HasValidSerial() const;

    bool Verify(const std::vector<PublicCoin>& anonymity_set, const SpendMetaData &m) const;

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(sigmaProof);
        READWRITE(coinSerialNumber);
        READWRITE(version);

        int64_t denomination_value = -1;
        if (ser_action.ForRead()) {
            READWRITE(denomination_value);
            IntegerToDenomination(denomination_value, this->denomination);
        } else {
            DenominationToInteger(this->denomination, denomination_value);
            READWRITE(denomination_value);
        }
        READWRITE(accumulatorBlockHash);
        READWRITE(ecdsaPubkey);
        READWRITE(ecdsaSignature);
    }
    
    uint256 signatureHash(const SpendMetaData& m) const;

private:
    const Params* params;
    unsigned int version = 0;
    CoinDenomination denomination;
    uint256 accumulatorBlockHash;
    Scalar coinSerialNumber;
    std::vector<unsigned char> ecdsaSignature;
    std::vector<unsigned char> ecdsaPubkey;
    SigmaPlusProof<Scalar, GroupElement> sigmaProof;

};

} //namespace sigma

#endif // SIGMA_COINSPEND_H
