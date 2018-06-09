// Copyright (c) 2017-2018 The Particl Core developers
// Copyright (c) 2018 The NIX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <amount.h>
#include <base58.h>
#include <chain.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <init.h>
#include <httpserver.h>
#include <validation.h>
#include <net.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <rpc/server.h>
#include <rpc/mining.h>
#include <rpc/safemode.h>
#include <rpc/util.h>
#include <script/sign.h>
#include <timedata.h>
#include <util.h>
#include <txdb.h>
#include <utilmoneystr.h>
#include <wallet/hd/hdwallet.h>
#include <wallet/hd/hdwalletdb.h>
#include <wallet/coincontrol.h>
#include <wallet/rpcwallet.h>
#include <chainparams.h>
#include <ghost-address/mnemonic.h>
#include <crypto/sha256.h>
#include <warnings.h>

#include <univalue.h>
#include <stdint.h>



void EnsureWalletIsUnlocked(CHDWallet *pwallet)
{
    if (pwallet->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Wallet locked, please enter the wallet passphrase with walletpassphrase first.");
};

static const std::string WALLET_ENDPOINT_BASE = "/wallet/";

CHDWallet *GetHDWalletForJSONRPCRequest(const JSONRPCRequest &request)
{
    if (request.URI.substr(0, WALLET_ENDPOINT_BASE.size()) == WALLET_ENDPOINT_BASE) {
        // wallet endpoint was used
        std::string requestedWallet = urlDecode(request.URI.substr(WALLET_ENDPOINT_BASE.size()));
        for (auto pwallet : ::vpwallets) {
            if (pwallet->GetName() == requestedWallet) {
                return GetHDWallet(pwallet);
            }
        }
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
    }
    return ::vpwallets.size() == 1 || (request.fHelp && ::vpwallets.size() > 0) ? GetHDWallet(::vpwallets[0]) : nullptr;
}

inline uint32_t reversePlace(const uint8_t *p)
{
    uint32_t rv = 0;
    for (int i = 0; i < 4; ++i)
        rv |= (uint32_t) *(p+i) << (8 * (3-i));
    return rv;
};

int ExtractBip32InfoV(const std::vector<uint8_t> &vchKey, UniValue &keyInfo, std::string &sError)
{
    CExtKey58 ek58;
    CExtKeyPair vk;
    vk.DecodeV(&vchKey[4]);

    CChainParams::Base58Type typePk = CChainParams::EXT_PUBLIC_KEY;
    if (memcmp(&vchKey[0], &Params().Base58Prefix(CChainParams::EXT_SECRET_KEY)[0], 4) == 0)
    {
        keyInfo.pushKV("type", "NIX extended secret key");
    } else
    if (memcmp(&vchKey[0], &Params().Base58Prefix(CChainParams::EXT_SECRET_KEY_BTC)[0], 4) == 0)
    {
        keyInfo.pushKV("type", "Bitcoin extended secret key");
        typePk = CChainParams::EXT_PUBLIC_KEY_BTC;
    } else
    {
        keyInfo.pushKV("type", "Unknown extended secret key");
    };

    keyInfo.pushKV("version", strprintf("%02X", reversePlace(&vchKey[0])));
    keyInfo.pushKV("depth", strprintf("%u", vchKey[4]));
    keyInfo.pushKV("parent_fingerprint", strprintf("%08X", reversePlace(&vchKey[5])));
    keyInfo.pushKV("child_index", strprintf("%u", reversePlace(&vchKey[9])));
    keyInfo.pushKV("chain_code", strprintf("%s", HexStr(&vchKey[13], &vchKey[13+32])));
    keyInfo.pushKV("key", strprintf("%s", HexStr(&vchKey[46], &vchKey[46+32])));

    // don't display raw secret ??
    // TODO: add option

    CKey key;
    key.Set(&vchKey[46], true);
    keyInfo.pushKV("privkey", strprintf("%s", CBitcoinSecret(key).ToString()));
    CKeyID id = key.GetPubKey().GetID();
    CBitcoinAddress addr;
    addr.Set(id, CChainParams::EXT_KEY_HASH);

    keyInfo.pushKV("id", addr.ToString().c_str());
    addr.Set(id);
    keyInfo.pushKV("address", addr.ToString().c_str());
    keyInfo.pushKV("checksum", strprintf("%02X", reversePlace(&vchKey[78])));

    ek58.SetKey(vk, typePk);
    keyInfo.pushKV("ext_public_key", ek58.ToString());

    return 0;
};

int ExtractBip32InfoP(const std::vector<uint8_t> &vchKey, UniValue &keyInfo, std::string &sError)
{
    CExtPubKey pk;

    if (memcmp(&vchKey[0], &Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY)[0], 4) == 0)
    {
        keyInfo.pushKV("type", "NIX extended public key");
    } else
    if (memcmp(&vchKey[0], &Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY_BTC)[0], 4) == 0)
    {
        keyInfo.pushKV("type", "Bitcoin extended public key");
    } else
    {
        keyInfo.pushKV("type", "Unknown extended public key");
    };

    keyInfo.pushKV("version", strprintf("%02X", reversePlace(&vchKey[0])));
    keyInfo.pushKV("depth", strprintf("%u", vchKey[4]));
    keyInfo.pushKV("parent_fingerprint", strprintf("%08X", reversePlace(&vchKey[5])));
    keyInfo.pushKV("child_index", strprintf("%u", reversePlace(&vchKey[9])));
    keyInfo.pushKV("chain_code", strprintf("%s", HexStr(&vchKey[13], &vchKey[13+32])));
    keyInfo.pushKV("key", strprintf("%s", HexStr(&vchKey[45], &vchKey[45+33])));

    CPubKey key;
    key.Set(&vchKey[45], &vchKey[78]);
    CKeyID id = key.GetID();
    CBitcoinAddress addr;
    addr.Set(id, CChainParams::EXT_KEY_HASH);

    keyInfo.pushKV("id", addr.ToString().c_str());
    addr.Set(id);
    keyInfo.pushKV("address", addr.ToString().c_str());
    keyInfo.pushKV("checksum", strprintf("%02X", reversePlace(&vchKey[78])));

    return 0;
};

int ExtKeyPathV(const std::string &sPath, const std::vector<uint8_t> &vchKey, UniValue &keyInfo, std::string &sError)
{
    if (sPath.compare("info") == 0)
        return ExtractBip32InfoV(vchKey, keyInfo, sError);

    CExtKey vk;
    vk.Decode(&vchKey[4]);
    CExtKey vkOut, vkWork = vk;

    std::vector<uint32_t> vPath;
    int rv;
    if ((rv = ExtractExtKeyPath(sPath, vPath)) != 0)
        return errorN(1, sError, __func__, "ExtractExtKeyPath failed %s", ExtKeyGetString(rv));

    for (std::vector<uint32_t>::iterator it = vPath.begin(); it != vPath.end(); ++it)
    {
        if (!vkWork.Derive(vkOut, *it))
            return errorN(1, sError, __func__, "CExtKey Derive failed");
        vkWork = vkOut;
    };

    CBitcoinExtKey ekOut;
    ekOut.SetKey(vkOut);
    keyInfo.pushKV("result", ekOut.ToString());

    // Display path, the quotes can go missing through the debug console. eg: m/44'/1', m/44\'/1\' works
    std::string sPathOut;
    if (0 != PathToString(vPath, sPathOut))
        return errorN(1, sError, __func__, "PathToString failed");
    keyInfo.pushKV("path", sPathOut);

    return 0;
};

int ExtKeyPathP(const std::string &sPath, const std::vector<uint8_t> &vchKey, UniValue &keyInfo, std::string &sError)
{
    if (sPath.compare("info") == 0)
        return ExtractBip32InfoP(vchKey, keyInfo, sError);

    CExtPubKey pk;
    pk.Decode(&vchKey[4]);

    CExtPubKey pkOut, pkWork = pk;

    std::vector<uint32_t> vPath;
    int rv;
    if ((rv = ExtractExtKeyPath(sPath, vPath)) != 0)
        return errorN(1, sError, __func__, "ExtractExtKeyPath failed %s", ExtKeyGetString(rv));

    for (std::vector<uint32_t>::iterator it = vPath.begin(); it != vPath.end(); ++it)
    {
        if ((*it >> 31) == 1)
            return errorN(1, sError, __func__, "Can't derive hardened keys from public ext key");
        if (!pkWork.Derive(pkOut, *it))
            return errorN(1, sError, __func__, "CExtKey Derive failed");
        pkWork = pkOut;
    };

    CBitcoinExtPubKey ekOut;
    ekOut.SetKey(pkOut);
    keyInfo.pushKV("result", ekOut.ToString());

    // Display path, the quotes can go missing through the debug console. eg: m/44'/1', m/44\'/1\' works
    std::string sPathOut;
    if (0 != PathToString(vPath, sPathOut))
        return errorN(1, sError, __func__, "PathToString failed");
    keyInfo.pushKV("path", sPathOut);

    return 0;
};

int AccountInfo(CHDWallet *pwallet, CExtKeyAccount *pa, int nShowKeys, bool fAllChains, UniValue &obj, std::string &sError)
{
    CExtKey58 eKey58;

    obj.pushKV("type", "Account");
    obj.pushKV("active", pa->nFlags & EAF_ACTIVE ? "true" : "false");
    obj.pushKV("label", pa->sLabel);

    if (pwallet->idDefaultAccount == pa->GetID())
        obj.pushKV("default_account", "true");

    mapEKValue_t::iterator mvi = pa->mapValue.find(EKVT_CREATED_AT);
    if (mvi != pa->mapValue.end())
    {
        int64_t nCreatedAt;
        GetCompressedInt64(mvi->second, (uint64_t&)nCreatedAt);
        obj.pushKV("created_at", nCreatedAt);
    };

    mvi = pa->mapValue.find(EKVT_HARDWARE_DEVICE);
    if (mvi != pa->mapValue.end())
    {
#if ENABLE_USBDEVICE

#endif
        if (mvi->second.size() >= 8)
        {
            int nVendorId = *((int*)mvi->second.data());
            int nProductId = *((int*)(mvi->second.data() + 4));
            obj.pushKV("hardware_device", strprintf("0x%04x 0x%04x", nVendorId, nProductId));
        };
    };

    obj.pushKV("id", pa->GetIDString58());
    obj.pushKV("has_secret", pa->nFlags & EAF_HAVE_SECRET ? "true" : "false");

    CStoredExtKey *sekAccount = pa->ChainAccount();
    if (!sekAccount)
    {
        obj.pushKV("error", "chain account not set.");
        return 0;
    };

    CBitcoinAddress addr;
    addr.Set(pa->idMaster, CChainParams::EXT_KEY_HASH);
    obj.pushKV("root_key_id", addr.ToString());

    mvi = sekAccount->mapValue.find(EKVT_PATH);
    if (mvi != sekAccount->mapValue.end())
    {
        std::string sPath;
        if (0 == PathToString(mvi->second, sPath, 'h'))
            obj.pushKV("path", sPath);
    };
    // TODO: separate passwords for accounts
    if (pa->nFlags & EAF_HAVE_SECRET
        && nShowKeys > 1
        && pwallet->ExtKeyUnlock(sekAccount) == 0)
    {
        eKey58.SetKeyV(sekAccount->kp);
        obj.pushKV("evkey", eKey58.ToString());
    };

    if (nShowKeys > 0)
    {
        eKey58.SetKeyP(sekAccount->kp);
        obj.pushKV("epkey", eKey58.ToString());
    };

    if (nShowKeys > 2) // dumpwallet
    {
        obj.pushKV("stealth_address_pack", (int)pa->nPackStealth);
        obj.pushKV("stealth_keys_received_pack", (int)pa->nPackStealthKeys);
    };


    if (fAllChains)
    {
        UniValue arChains(UniValue::VARR);
        for (size_t i = 1; i < pa->vExtKeys.size(); ++i) // vExtKeys[0] stores the account key
        {
            UniValue objC(UniValue::VOBJ);
            CStoredExtKey *sek = pa->vExtKeys[i];
            eKey58.SetKeyP(sek->kp);

            if (pa->nActiveExternal == i)
                objC.pushKV("function", "active_external");
            if (pa->nActiveInternal == i)
                objC.pushKV("function", "active_internal");
            if (pa->nActiveStealth == i)
                objC.pushKV("function", "active_stealth");

            objC.pushKV("id", sek->GetIDString58());
            objC.pushKV("chain", eKey58.ToString());
            objC.pushKV("label", sek->sLabel);
            objC.pushKV("active", sek->nFlags & EAF_ACTIVE ? "true" : "false");
            objC.pushKV("receive_on", sek->nFlags & EAF_RECEIVE_ON ? "true" : "false");

            mapEKValue_t::iterator it = sek->mapValue.find(EKVT_KEY_TYPE);
            if (it != sek->mapValue.end() && it->second.size() > 0)
            {
                std::string(sUseType);
                switch(it->second[0])
                {
                    case EKT_EXTERNAL:      sUseType = "external";      break;
                    case EKT_INTERNAL:      sUseType = "internal";      break;
                    case EKT_STEALTH:       sUseType = "stealth";       break;
                    case EKT_CONFIDENTIAL:  sUseType = "confidential";  break;
                    case EKT_STEALTH_SCAN:  sUseType = "stealth_scan";  break;
                    case EKT_STEALTH_SPEND: sUseType = "stealth_spend"; break;
                    default:                sUseType = "unknown";       break;
                };
                objC.pushKV("use_type", sUseType);
            };

            objC.pushKV("num_derives", strprintf("%u", sek->nGenerated));
            objC.pushKV("num_derives_h", strprintf("%u", sek->nHGenerated));

            if (nShowKeys > 2 // dumpwallet
                && pa->nFlags & EAF_HAVE_SECRET)
            {
                eKey58.SetKeyV(sek->kp);
                objC.pushKV("evkey", eKey58.ToString());

                mvi = sek->mapValue.find(EKVT_CREATED_AT);
                if (mvi != sek->mapValue.end())
                {
                    int64_t nCreatedAt;
                    GetCompressedInt64(mvi->second, (uint64_t&)nCreatedAt);
                    objC.pushKV("created_at", nCreatedAt);
                };
            };

            mvi = sek->mapValue.find(EKVT_PATH);
            if (mvi != sek->mapValue.end())
            {
                std::string sPath;
                if (0 == PathToString(mvi->second, sPath, 'h'))
                    objC.pushKV("path", sPath);
            };

            arChains.push_back(objC);
        };
        obj.pushKV("chains", arChains);
    } else
    {
        if (pa->nActiveExternal < pa->vExtKeys.size())
        {
            CStoredExtKey *sekE = pa->vExtKeys[pa->nActiveExternal];
            if (nShowKeys > 0)
            {
                eKey58.SetKeyP(sekE->kp);
                obj.pushKV("external_chain", eKey58.ToString());
            };
            obj.pushKV("num_derives_external", strprintf("%u", sekE->nGenerated));
            obj.pushKV("num_derives_external_h", strprintf("%u", sekE->nHGenerated));
        };

        if (pa->nActiveInternal < pa->vExtKeys.size())
        {
            CStoredExtKey *sekI = pa->vExtKeys[pa->nActiveInternal];
            if (nShowKeys > 0)
            {
                eKey58.SetKeyP(sekI->kp);
                obj.pushKV("internal_chain", eKey58.ToString());
            };
            obj.pushKV("num_derives_internal", strprintf("%u", sekI->nGenerated));
            obj.pushKV("num_derives_internal_h", strprintf("%u", sekI->nHGenerated));
        };

        if (pa->nActiveStealth < pa->vExtKeys.size())
        {
            CStoredExtKey *sekS = pa->vExtKeys[pa->nActiveStealth];
            obj.pushKV("num_derives_stealth", strprintf("%u", sekS->nGenerated));
            obj.pushKV("num_derives_stealth_h", strprintf("%u", sekS->nHGenerated));
        };
    };

    return 0;
};

int AccountInfo(CHDWallet *pwallet, CKeyID &keyId, int nShowKeys, bool fAllChains, UniValue &obj, std::string &sError)
{
    // TODO: inactive keys can be in db and not in memory - search db for keyId
    ExtKeyAccountMap::iterator mi = pwallet->mapExtAccounts.find(keyId);
    if (mi == pwallet->mapExtAccounts.end())
    {
        sError = "Unknown account.";
        return 1;
    };

    CExtKeyAccount *pa = mi->second;
    return AccountInfo(pwallet, pa, nShowKeys, fAllChains, obj, sError);
};

int KeyInfo(CHDWallet *pwallet, CKeyID &idMaster, CKeyID &idKey, CStoredExtKey &sek, int nShowKeys, UniValue &obj, std::string &sError)
{
    CExtKey58 eKey58;

    bool fBip44Root = false;
    obj.pushKV("type", "Loose");
    obj.pushKV("active", sek.nFlags & EAF_ACTIVE ? "true" : "false");
    obj.pushKV("receive_on", sek.nFlags & EAF_RECEIVE_ON ? "true" : "false");
    obj.pushKV("encrypted", sek.nFlags & EAF_IS_CRYPTED ? "true" : "false");
    obj.pushKV("hardware_device", sek.nFlags & EAF_HARDWARE_DEVICE ? "true" : "false");
    obj.pushKV("label", sek.sLabel);

    if (reversePlace(&sek.kp.vchFingerprint[0]) == 0)
    {
        obj.pushKV("path", "Root");
    } else
    {
        mapEKValue_t::iterator mvi = sek.mapValue.find(EKVT_PATH);
        if (mvi != sek.mapValue.end())
        {
            std::string sPath;
            if (0 == PathToString(mvi->second, sPath, 'h'))
                obj.pushKV("path", sPath);
        };
    };

    mapEKValue_t::iterator mvi = sek.mapValue.find(EKVT_KEY_TYPE);
    if (mvi != sek.mapValue.end())
    {
        uint8_t type = EKT_MAX_TYPES;
        if (mvi->second.size() == 1)
            type = mvi->second[0];

        std::string sType;
        switch (type)
        {
            case EKT_MASTER      : sType = "Master"; break;
            case EKT_BIP44_MASTER:
                sType = "BIP44 Root Key";
                fBip44Root = true;
                break;
            default              : sType = "Unknown"; break;
        };
        obj.pushKV("key_type", sType);
    };

    if (idMaster == idKey)
        obj.pushKV("current_master", "true");

    CBitcoinAddress addr;
    mvi = sek.mapValue.find(EKVT_ROOT_ID);
    if (mvi != sek.mapValue.end())
    {
        CKeyID idRoot;

        if (GetCKeyID(mvi->second, idRoot))
        {
            addr.Set(idRoot, CChainParams::EXT_KEY_HASH);
            obj.pushKV("root_key_id", addr.ToString());
        } else
        {
            obj.pushKV("root_key_id", "malformed");
        };
    };

    mvi = sek.mapValue.find(EKVT_CREATED_AT);
    if (mvi != sek.mapValue.end())
    {
        int64_t nCreatedAt;
        GetCompressedInt64(mvi->second, (uint64_t&)nCreatedAt);
        obj.pushKV("created_at", nCreatedAt);
    };

    addr.Set(idKey, CChainParams::EXT_KEY_HASH);
    obj.pushKV("id", addr.ToString());


    if (nShowKeys > 1
        && pwallet->ExtKeyUnlock(&sek) == 0)
    {
        std::string sKey;
        if (sek.kp.IsValidV())
        {
            if (fBip44Root)
                eKey58.SetKey(sek.kp, CChainParams::EXT_SECRET_KEY_BTC);
            else
                eKey58.SetKeyV(sek.kp);
            sKey = eKey58.ToString();
        } else
        {
            sKey = "Unknown";
        };

        obj.pushKV("evkey", sKey);
    };

    if (nShowKeys > 0)
    {
        if (fBip44Root)
            eKey58.SetKey(sek.kp, CChainParams::EXT_PUBLIC_KEY_BTC);
        else
            eKey58.SetKeyP(sek.kp);

        obj.pushKV("epkey", eKey58.ToString());
    };

    obj.pushKV("num_derives", strprintf("%u", sek.nGenerated));
    obj.pushKV("num_derives_hardened", strprintf("%u", sek.nHGenerated));

    return 0;
};

int KeyInfo(CHDWallet *pwallet, CKeyID &idMaster, CKeyID &idKey, int nShowKeys, UniValue &obj, std::string &sError)
{
    CStoredExtKey sek;
    {
        LOCK(pwallet->cs_wallet);
        CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");

        if (!wdb.ReadExtKey(idKey, sek))
        {
            sError = "Key not found in wallet.";
            return 1;
        };
    }

    return KeyInfo(pwallet, idMaster, idKey, sek, nShowKeys, obj, sError);
};

class ListExtCallback : public LoopExtKeyCallback
{
public:
    ListExtCallback(CHDWallet *pwalletIn, UniValue *arr, int _nShowKeys)
    {
        pwallet = pwalletIn;
        nItems = 0;
        rvArray = arr;
        nShowKeys = _nShowKeys;

        if (pwallet && pwallet->pEKMaster)
            idMaster = pwallet->pEKMaster->GetID();
    };

    int ProcessKey(CKeyID &id, CStoredExtKey &sek)
    {
        nItems++;
        UniValue obj(UniValue::VOBJ);
        if (0 != KeyInfo(pwallet, idMaster, id, sek, nShowKeys, obj, sError))
        {
            obj.pushKV("id", sek.GetIDString58());
            obj.pushKV("error", sError);
        };

        rvArray->push_back(obj);
        return 0;
    };

    int ProcessAccount(CKeyID &id, CExtKeyAccount &sea)
    {
        nItems++;
        UniValue obj(UniValue::VOBJ);

        bool fAllChains = nShowKeys > 2 ? true : false;
        if (0 != AccountInfo(pwallet, &sea, nShowKeys, fAllChains, obj, sError))
        {
            obj.pushKV("id", sea.GetIDString58());
            obj.pushKV("error", sError);
        };

        rvArray->push_back(obj);
        return 0;
    };

    std::string sError;
    int nItems;
    int nShowKeys;
    CKeyID idMaster;
    UniValue *rvArray;
};

int ListLooseExtKeys(CHDWallet *pwallet, int nShowKeys, UniValue &ret, size_t &nKeys)
{
    ListExtCallback cbc(pwallet, &ret, nShowKeys);

    if (0 != LoopExtKeysInDB(pwallet, true, false, cbc))
        return errorN(1, "LoopExtKeys failed.");

    nKeys = cbc.nItems;

    return 0;
};

int ListAccountExtKeys(CHDWallet *pwallet, int nShowKeys, UniValue &ret, size_t &nKeys)
{
    ListExtCallback cbc(pwallet, &ret, nShowKeys);

    if (0 != LoopExtAccountsInDB(pwallet, true, cbc))
        return errorN(1, "LoopExtKeys failed.");

    nKeys = cbc.nItems;

    return 0;
};

int ManageExtKey(CStoredExtKey &sek, std::string &sOptName, std::string &sOptValue, UniValue &result, std::string &sError)
{
    if (sOptName == "label")
    {
        if (sOptValue.length() == 0)
            sek.sLabel = sOptValue;

        result.pushKV("set_label", sek.sLabel);
    } else
    if (sOptName == "active")
    {
        if (sOptValue.length() > 0)
        {
            if (nix::IsStringBoolPositive(sOptValue))
                sek.nFlags |= EAF_ACTIVE;
            else
                sek.nFlags &= ~EAF_ACTIVE;
        };

        result.pushKV("set_active", sek.nFlags & EAF_ACTIVE ? "true" : "false");
    } else
    if (sOptName == "receive_on")
    {
        if (sOptValue.length() > 0)
        {
            if (nix::IsStringBoolPositive(sOptValue))
                sek.nFlags |= EAF_RECEIVE_ON;
            else
                sek.nFlags &= ~EAF_RECEIVE_ON;
        };

        result.pushKV("receive_on", sek.nFlags & EAF_RECEIVE_ON ? "true" : "false");
    } else
    if (sOptName == "look_ahead")
    {
        uint64_t nLookAhead = gArgs.GetArg("-defaultlookaheadsize", N_DEFAULT_LOOKAHEAD);

        if (sOptValue.length() > 0)
        {
            char *pend;
            errno = 0;
            nLookAhead = strtoul(sOptValue.c_str(), &pend, 10);
            if (errno != 0 || !pend || *pend != '\0')
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Failed: look_ahead invalid number.");

            if (nLookAhead < 1 || nLookAhead > 1000)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Failed: look_ahead number out of range.");

            std::vector<uint8_t> v;
            sek.mapValue[EKVT_N_LOOKAHEAD] = SetCompressedInt64(v, nLookAhead);
            result.pushKV("note", "Wallet must be restarted to reload lookahead pool.");
        };

        mapEKValue_t::iterator itV = sek.mapValue.find(EKVT_N_LOOKAHEAD);
        if (itV != sek.mapValue.end())
        {
            nLookAhead = GetCompressedInt64(itV->second, nLookAhead);
            result.pushKV("look_ahead", (int)nLookAhead);
        } else
        {
            result.pushKV("look_ahead", "default");
        };
    } else
    {
        // List all possible
        result.pushKV("label", sek.sLabel);
        result.pushKV("active", sek.nFlags & EAF_ACTIVE ? "true" : "false");
        result.pushKV("receive_on", sek.nFlags & EAF_RECEIVE_ON ? "true" : "false");


        mapEKValue_t::iterator itV = sek.mapValue.find(EKVT_N_LOOKAHEAD);
        if (itV != sek.mapValue.end())
        {
            uint64_t nLookAhead = GetCompressedInt64(itV->second, nLookAhead);
            result.pushKV("look_ahead", (int)nLookAhead);
        } else
        {
            result.pushKV("look_ahead", "default");
        };
    };

    return 0;
};

int ManageExtAccount(CExtKeyAccount &sea, std::string &sOptName, std::string &sOptValue, UniValue &result, std::string &sError)
{
    if (sOptName == "label")
    {
        if (sOptValue.length() > 0)
            sea.sLabel = sOptValue;

        result.pushKV("set_label", sea.sLabel);
    } else
    if (sOptName == "active")
    {
        if (sOptValue.length() > 0)
        {
            if (nix::IsStringBoolPositive(sOptValue))
                sea.nFlags |= EAF_ACTIVE;
            else
                sea.nFlags &= ~EAF_ACTIVE;
        };

        result.pushKV("set_active", sea.nFlags & EAF_ACTIVE ? "true" : "false");
    } else
    {
        // List all possible
        result.pushKV("label", sea.sLabel);
        result.pushKV("active", sea.nFlags & EAF_ACTIVE ? "true" : "false");
    };

    return 0;
};

static int ExtractExtKeyId(const std::string &sInKey, CKeyID &keyId, CChainParams::Base58Type prefix)
{
    CExtKey58 eKey58;
    CExtKeyPair ekp;
    CBitcoinAddress addr;

    if (addr.SetString(sInKey)
        && addr.IsValid(prefix)
        && addr.GetKeyID(keyId, prefix))
    {
        // keyId is set
    } else
    if (eKey58.Set58(sInKey.c_str()) == 0)
    {
        ekp = eKey58.GetKey();
        keyId = ekp.GetID();
    } else
    {
        throw std::runtime_error("Invalid key.");
    };
    return 0;
};

UniValue extkey(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    static const char *help = ""
        "extkey \"mode\"\n"
        "\"mode\" can be: info|list|account|gen|import|importAccount|setMaster|setDefaultAccount|deriveAccount|options\n"
        "    Default: list, or info when called like: extkey \"key\"\n"
        "\n"
        "extkey info \"key\" ( \"path\" )\n"
        "    Return info for provided \"key\" or key at \"path\" from \"key\"\n"
        "extkey list ( show_secrets )\n"
        "    List loose and account ext keys.\n"
        "extkey account ( \"key/id\" show_secrets )\n"
        "    Display details of account.\n"
        "    Show default account when called without parameters.\n"
        "extkey key \"key/id\" ( show_secrets )\n"
        "    Display details of loose extkey in wallet.\n"
        "extkey gen \"passphrase\" ( numhashes \"seedstring\" )\n"
        "    DEPRECATED\n"
        "    If no passhrase is specified key will be generated from random data.\n"
        "    Warning: It is recommended to not use the passphrase\n"
        "extkey import \"key\" ( \"label\" bip44 save_bip44_key )\n"
        "    Add loose key to wallet.\n"
        "    If bip44 is set import will add the key derived from <key> on the bip44 path.\n"
        "    If save_bip44_key is set import will save the bip44 key to the wallet.\n"
        "extkey importAccount \"key\" ( time_scan_from \"label\" ) \n"
        "    Add account key to wallet.\n"
        "        time_scan_from: N no check, Y-m-d date to start scanning the blockchain for owned txns.\n"
        "extkey setMaster \"key/id\"\n"
        "    Set a private ext key as current master key.\n"
        "    key can be a extkeyid or full key, but must be in the wallet.\n"
        "extkey setDefaultAccount \"id\"\n"
        "    Set an account as the default.\n"
        "extkey deriveAccount ( \"label\" \"path\" )\n"
        "    Make a new account from the current master key, save to wallet.\n"
        "extkey options \"key\" ( \"optionName\" \"newValue\" )\n"
        "    Manage keys and accounts\n"
        "\n";

    ObserveSafeMode();

    // default mode is list unless 1st parameter is a key - then mode is set to info

    // path:
    // master keys are hashed with an integer (child_index) to form child keys
    // each child key can spawn more keys
    // payments etc are not send to keys derived from the master keys
    //  m - master key
    //  m/0 - key0 (1st) key derived from m
    //  m/1/2 key2 (3rd) key derived from key1 derived from m

    // hardened keys are keys with (child_index) > 2^31
    // it's not possible to compute the next extended public key in the sequence from a hardened public key (still possible with a hardened private key)

    // this maintains privacy, you can give hardened public keys to customers
    // and they will not be able to compute/guess the key you give out to other customers
    // but will still be able to send payments to you on the 2^32 keys derived from the public key you provided


    // accounts to receive must be non-hardened
    //   - locked wallets must be able to derive new keys as they receive

    if (request.fHelp || request.params.size() > 5) // defaults to info, will always take at least 1 parameter
        throw std::runtime_error(help);

    EnsureWalletIsUnlocked(pwallet);

    std::string mode = "list";
    std::string sInKey = "";

    uint32_t nParamOffset = 0;
    if (request.params.size() > 0)
    {
        std::string s = request.params[0].get_str();
        std::string st = " " + s + " "; // Note the spaces
        std::transform(st.begin(), st.end(), st.begin(), ::tolower);
        static const char *pmodes = " info list gen account key import importaccount setmaster setdefaultaccount deriveaccount options ";
        if (strstr(pmodes, st.c_str()) != nullptr)
        {
            st.erase(std::remove(st.begin(), st.end(), ' '), st.end());
            mode = st;
            nParamOffset = 1;
        } else
        {
            sInKey = s;
            mode = "info";
            nParamOffset = 1;
        };
    };

    CBitcoinExtKey bvk;
    CBitcoinExtPubKey bpk;

    std::vector<uint8_t> vchVersionIn;
    vchVersionIn.resize(4);

    UniValue result(UniValue::VOBJ);

    if (mode == "info")
    {
        std::string sMode = "info"; // info lists details of bip32 key, m displays internal key

        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            };
        };

        if (request.params.size() > nParamOffset)
            sMode = request.params[nParamOffset].get_str();

        UniValue keyInfo(UniValue::VOBJ);
        std::vector<uint8_t> vchOut;

        if (!DecodeBase58(sInKey.c_str(), vchOut))
            throw std::runtime_error("DecodeBase58 failed.");
        if (!VerifyChecksum(vchOut))
            throw std::runtime_error("VerifyChecksum failed.");

        size_t keyLen = vchOut.size();
        std::string sError;

        if (keyLen != BIP32_KEY_LEN)
            throw std::runtime_error(strprintf("Unknown ext key length '%d'", keyLen));

        if (memcmp(&vchOut[0], &Params().Base58Prefix(CChainParams::EXT_SECRET_KEY)[0], 4) == 0
            || memcmp(&vchOut[0], &Params().Base58Prefix(CChainParams::EXT_SECRET_KEY_BTC)[0], 4) == 0)
        {
            if (ExtKeyPathV(sMode, vchOut, keyInfo, sError) != 0)
                throw std::runtime_error(strprintf("ExtKeyPathV failed %s.", sError.c_str()));
        } else
        if (memcmp(&vchOut[0], &Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY)[0], 4) == 0
            || memcmp(&vchOut[0], &Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY_BTC)[0], 4) == 0)
        {
            if (ExtKeyPathP(sMode, vchOut, keyInfo, sError) != 0)
                throw std::runtime_error(strprintf("ExtKeyPathP failed %s.", sError.c_str()));
        } else
        {
            throw std::runtime_error(strprintf("Unknown prefix '%s'", sInKey.substr(0, 4)));
        };

        result.pushKV("key_info", keyInfo);
    } else
    if (mode == "list")
    {
        UniValue ret(UniValue::VARR);

        int nListFull = 0; // 0 id only, 1 id+pubkey, 2 id+pubkey+secret
        if (request.params.size() > nParamOffset)
        {
            std::string st = request.params[nParamOffset].get_str();
            if (nix::IsStringBoolPositive(st))
                nListFull = 2;

            nParamOffset++;
        };

        size_t nKeys = 0, nAcc = 0;

        {
            LOCK(pwallet->cs_wallet);
            ListLooseExtKeys(pwallet, nListFull, ret, nKeys);
            ListAccountExtKeys(pwallet, nListFull, ret, nAcc);
        } // cs_wallet

        if (nKeys + nAcc > 0)
            return ret;

        result.pushKV("result", "No keys to list.");
    } else
    if (mode == "account"
        || mode == "key")
    {
        CKeyID keyId;
        if (request.params.size() > nParamOffset)
        {
            sInKey = request.params[nParamOffset].get_str();
            nParamOffset++;

            if (mode == "account" && sInKey == "default")
                keyId = pwallet->idDefaultAccount;
            else
                ExtractExtKeyId(sInKey, keyId, mode == "account" ? CChainParams::EXT_ACC_HASH : CChainParams::EXT_KEY_HASH);
        } else
        {
            if (mode == "account")
            {
                // Display default account
                keyId = pwallet->idDefaultAccount;
            };
        };
        if (keyId.IsNull())
            throw std::runtime_error(strprintf("Must specify ext key or id %s.", mode == "account" ? "or 'default'" : ""));

        int nListFull = 0; // 0 id only, 1 id+pubkey, 2 id+pubkey+secret
        if (request.params.size() > nParamOffset)
        {
            std::string st = request.params[nParamOffset].get_str();
            if (nix::IsStringBoolPositive(st))
                nListFull = 2;

            nParamOffset++;
        };

        std::string sError;
        if (mode == "account")
        {
            if (0 != AccountInfo(pwallet, keyId, nListFull, true, result, sError))
                throw std::runtime_error("AccountInfo failed: " + sError);
        } else
        {
            CKeyID idMaster;
            if (pwallet->pEKMaster)
                idMaster = pwallet->pEKMaster->GetID();
            else
                LogPrintf("%s: Warning: Master key isn't set!\n", __func__);
            if (0 != KeyInfo(pwallet, idMaster, keyId, nListFull, result, sError))
                throw std::runtime_error("KeyInfo failed: " + sError);
        };
    } else
    if (mode == "gen")
    {
        // Make a new master key
        // from random or passphrase + int + seed string

        CExtKey newKey;
        CBitcoinExtKey b58Key;

        if (request.params.size() > 1)
        {
            std::string sPassphrase = request.params[1].get_str();
            int32_t nHashes = 100;
            std::string sSeed = "Bitcoin seed";

            // Generate from passphrase
            //   allow generator string and nhashes to be specified
            //   To allow importing of bip32 strings from other systems
            //   Match bip32.org: bip32 gen "pass" 50000 "Bitcoin seed"

            if (request.params.size() > 2)
            {
                std::stringstream sstr(request.params[2].get_str());

                sstr >> nHashes;
                if (!sstr)
                    throw std::runtime_error("Invalid num hashes");

                if (nHashes < 1)
                    throw std::runtime_error("Num hashes must be 1 or more.");
            };

            if (request.params.size() > 3)
            {
                sSeed = request.params[3].get_str();
            };

            if (request.params.size() > 4)
                throw std::runtime_error(help);

            pwallet->ExtKeyNew32(newKey, sPassphrase.c_str(), nHashes, sSeed.c_str());

            result.pushKV("warning",
                "If the same passphrase is used by another your privacy and coins will be compromised.\n"
                "It is not recommended to use this feature - if you must, pick very unique values for passphrase, num hashes and generator parameters.");
        } else
        {
             pwallet->ExtKeyNew32(newKey);
        };

        b58Key.SetKey(newKey);

        result.pushKV("result", b58Key.ToString());
    } else
    if (mode == "import")
    {
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            };
        };

        CStoredExtKey sek;
        if (request.params.size() > nParamOffset)
        {
            sek.sLabel = request.params[nParamOffset].get_str();
            nParamOffset++;
        };

        bool fBip44 = false;
        if (request.params.size() > nParamOffset)
        {
            std::string req;
            if(request.params[nParamOffset].isBool())
                fBip44 = request.params[nParamOffset].getBool();
            else{
                req = request.params[nParamOffset].get_str();
                if (!nix::GetStringBool(req, fBip44))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Not a bool value.");

            }
            nParamOffset++;
        };

        bool fSaveBip44 = false;
        if (request.params.size() > nParamOffset)
        {
            std::string req;
            if(request.params[nParamOffset].isBool())
                fSaveBip44 = request.params[nParamOffset].getBool();
            else{
                req = request.params[nParamOffset].get_str();
                if (!nix::GetStringBool(req, fSaveBip44))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Not a bool value.");

            }
            nParamOffset++;
        };

        std::vector<uint8_t> v;
        sek.mapValue[EKVT_CREATED_AT] = SetCompressedInt64(v, GetTime());

        CExtKey58 eKey58;
        if (eKey58.Set58(sInKey.c_str()) != 0)
            throw std::runtime_error("Import failed - Invalid key.");

        if (fBip44)
        {
            if (!eKey58.IsValid(CChainParams::EXT_SECRET_KEY_BTC))
                throw std::runtime_error("Import failed - BIP44 key must begin with a bitcoin secret key prefix.");
        } else
        {
            if (!eKey58.IsValid(CChainParams::EXT_SECRET_KEY)
                && !eKey58.IsValid(CChainParams::EXT_PUBLIC_KEY_BTC))
                throw std::runtime_error("Import failed - Key must begin with a NIX prefix.");
        };

        sek.kp = eKey58.GetKey();

        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");

            int rv;
            CKeyID idDerived;
            if (0 != (rv = pwallet->ExtKeyImportLoose(&wdb, sek, idDerived, fBip44, fSaveBip44)))
            {
                wdb.TxnAbort();
                throw std::runtime_error(strprintf("ExtKeyImportLoose failed, %s", ExtKeyGetString(rv)));
            };

            if (!wdb.TxnCommit())
                throw std::runtime_error("TxnCommit failed.");

            CBitcoinAddress addr;
            addr.Set(fBip44 ? idDerived : sek.GetID(), CChainParams::EXT_KEY_HASH);
            result.pushKV("result", "Success.");
            result.pushKV("id", addr.ToString().c_str());
            result.pushKV("key_label", sek.sLabel);
            result.pushKV("note", "Please backup your wallet."); // TODO: check for child of existing key?
        } // cs_wallet
    } else
    if (mode == "importaccount")
    {
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            };
        };

        int64_t nTimeStartScan = 1; // scan from start, 0 means no scan
        if (request.params.size() > nParamOffset)
        {
            std::string sVar = request.params[nParamOffset].get_str();
            nParamOffset++;

            if (sVar == "N")
            {
                nTimeStartScan = 0;
            } else
            if (nix::IsStrOnlyDigits(sVar))
            {
                // Setting timestamp directly
                errno = 0;
                nTimeStartScan = strtoimax(sVar.c_str(), nullptr, 10);
                if (errno != 0)
                    throw std::runtime_error("Import Account failed - Parse time error.");
            } else
            {
                int year, month, day;

                if (sscanf(sVar.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
                    throw std::runtime_error("Import Account failed - Parse time error.");

                struct tm tmdate;
                tmdate.tm_year = year - 1900;
                tmdate.tm_mon = month - 1;
                tmdate.tm_mday = day;
                time_t t = mktime(&tmdate);

                nTimeStartScan = t;
            };
        };

        int64_t nCreatedAt = nTimeStartScan ? nTimeStartScan : GetTime();

        std::string sLabel;
        if (request.params.size() > nParamOffset)
        {
            sLabel = request.params[nParamOffset].get_str();
            nParamOffset++;
        };

        CStoredExtKey sek;
        CExtKey58 eKey58;
        if (eKey58.Set58(sInKey.c_str()) == 0)
        {
            sek.kp = eKey58.GetKey();
        } else
        {
            throw std::runtime_error("Import Account failed - Invalid key.");
        };

        {
            WalletRescanReserver reserver(pwallet);
            if (!reserver.reserve()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
            }

            LOCK2(cs_main, pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");

            int rv = pwallet->ExtKeyImportAccount(&wdb, sek, nCreatedAt, sLabel);
            if (rv == 1)
            {
                wdb.TxnAbort();
                throw std::runtime_error("Import failed - ExtKeyImportAccount failed.");
            } else
            if (rv == 2)
            {
                wdb.TxnAbort();
                throw std::runtime_error("Import failed - account exists.");
            } else
            {
                if (!wdb.TxnCommit())
                    throw std::runtime_error("TxnCommit failed.");
                result.pushKV("result", "Success.");

                if (rv == 3)
                    result.pushKV("result", "secret added to existing account.");

                result.pushKV("account_label", sLabel);
                result.pushKV("scanned_from", nTimeStartScan);
                result.pushKV("note", "Please backup your wallet."); // TODO: check for child of existing key?
            };

            pwallet->RescanFromTime(nTimeStartScan, reserver, true /* update */);
            pwallet->MarkDirty();
            pwallet->ReacceptWalletTransactions();

        } // cs_wallet
    } else
    if (mode == "setmaster")
    {
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            } else
                throw std::runtime_error("Must specify ext key or id.");
        };

        CKeyID idNewMaster;
        ExtractExtKeyId(sInKey, idNewMaster, CChainParams::EXT_KEY_HASH);

        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");

            int rv;
            if (0 != (rv = pwallet->ExtKeySetMaster(&wdb, idNewMaster)))
            {
                wdb.TxnAbort();
                throw std::runtime_error(strprintf("ExtKeySetMaster failed, %s.", ExtKeyGetString(rv)));
            };
            if (!wdb.TxnCommit())
                throw std::runtime_error("TxnCommit failed.");
            result.pushKV("result", "Success.");
        } // cs_wallet

    } else
    if (mode == "setdefaultaccount")
    {
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            } else
                throw std::runtime_error("Must specify ext key or id.");
        };

        CKeyID idNewDefault;
        CKeyID idOldDefault = pwallet->idDefaultAccount;
        CBitcoinAddress addr;

        CExtKeyAccount *sea = new CExtKeyAccount();

        if (addr.SetString(sInKey)
            && addr.IsValid(CChainParams::EXT_ACC_HASH)
            && addr.GetKeyID(idNewDefault, CChainParams::EXT_ACC_HASH))
        {
            // idNewDefault is set
        };

        int rv;
        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");

            if (!wdb.TxnBegin())
            {
                delete sea;
                throw std::runtime_error("TxnBegin failed.");
            };
            if (0 != (rv = pwallet->ExtKeySetDefaultAccount(&wdb, idNewDefault)))
            {
                delete sea;
                wdb.TxnAbort();
                throw std::runtime_error(strprintf("ExtKeySetDefaultAccount failed, %s.", ExtKeyGetString(rv)));
            };
            if (!wdb.TxnCommit())
            {
                delete sea;
                pwallet->idDefaultAccount = idOldDefault;
                throw std::runtime_error("TxnCommit failed.");
            };

            result.pushKV("result", "Success.");
        } // cs_wallet

    } else
    if (mode == "deriveaccount")
    {
        std::string sLabel, sPath;
        if (request.params.size() > nParamOffset)
        {
            sLabel = request.params[nParamOffset].get_str();
            nParamOffset++;
        };

        if (request.params.size() > nParamOffset)
        {
            sPath = request.params[nParamOffset].get_str();
            nParamOffset++;
        };

        for (; nParamOffset < request.params.size(); nParamOffset++)
        {
            std::string strParam = request.params[nParamOffset].get_str();
            std::transform(strParam.begin(), strParam.end(), strParam.begin(), ::tolower);

            throw std::runtime_error(strprintf("Unknown parameter '%s'", strParam.c_str()));
        };

        CExtKeyAccount *sea = new CExtKeyAccount();

        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");

            int rv;
            if ((rv = pwallet->ExtKeyDeriveNewAccount(&wdb, sea, sLabel, sPath)) != 0)
            {
                wdb.TxnAbort();
                result.pushKV("result", "Failed.");
                result.pushKV("reason", ExtKeyGetString(rv));
            } else
            {
                if (!wdb.TxnCommit())
                    throw std::runtime_error("TxnCommit failed.");

                result.pushKV("result", "Success.");
                result.pushKV("account", sea->GetIDString58());
                CStoredExtKey *sekAccount = sea->ChainAccount();
                if (sekAccount)
                {
                    CExtKey58 eKey58;
                    eKey58.SetKeyP(sekAccount->kp);
                    result.pushKV("public key", eKey58.ToString());
                };

                if (sLabel != "")
                    result.pushKV("label", sLabel);
            };
        } // cs_wallet
    } else
    if (mode == "options")
    {
        std::string sOptName, sOptValue, sError;
        if (sInKey.length() == 0)
        {
            if (request.params.size() > nParamOffset)
            {
                sInKey = request.params[nParamOffset].get_str();
                nParamOffset++;
            } else
                throw std::runtime_error("Must specify ext key or id.");
        };
        if (request.params.size() > nParamOffset)
        {
            sOptName = request.params[nParamOffset].get_str();
            nParamOffset++;
        };
        if (request.params.size() > nParamOffset)
        {
            sOptValue = request.params[nParamOffset].get_str();
            nParamOffset++;
        };

        CBitcoinAddress addr;

        CKeyID id;
        if (!addr.SetString(sInKey))
            throw std::runtime_error("Invalid key or account id.");

        bool fAccount = false;
        bool fKey = false;
        if (addr.IsValid(CChainParams::EXT_KEY_HASH)
            && addr.GetKeyID(id, CChainParams::EXT_KEY_HASH))
        {
            // id is set
            fKey = true;
        } else
        if (addr.IsValid(CChainParams::EXT_ACC_HASH)
            && addr.GetKeyID(id, CChainParams::EXT_ACC_HASH))
        {
            // id is set
            fAccount = true;
        } else
        if (addr.IsValid(CChainParams::EXT_PUBLIC_KEY))
        {
            CExtKeyPair ek = boost::get<CExtKeyPair>(addr.Get());

            id = ek.GetID();

            ExtKeyAccountMap::iterator it = pwallet->mapExtAccounts.find(id);
            if (it != pwallet->mapExtAccounts.end())
                fAccount = true;
            else
                fKey = true;
        } else
        {
            throw std::runtime_error("Invalid key or account id.");
        };

        CStoredExtKey sek;
        CExtKeyAccount sea;
        {
            LOCK(pwallet->cs_wallet);
            CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");
            if (!wdb.TxnBegin())
                throw std::runtime_error("TxnBegin failed.");

            if (fKey)
            {
                // Try key in memory first
                CStoredExtKey *pSek;
                ExtKeyMap::iterator it = pwallet->mapExtKeys.find(id);
                if (it != pwallet->mapExtKeys.end())
                {
                    pSek = it->second;
                } else
                if (wdb.ReadExtKey(id, sek))
                {
                    pSek = &sek;
                } else
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Key not in wallet.");
                };

                if (0 != ManageExtKey(*pSek, sOptName, sOptValue, result, sError))
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Error: " + sError);
                };

                if (sOptValue.length() > 0
                    && !wdb.WriteExtKey(id, *pSek))
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("WriteExtKey failed.");
                };
            };

            if (fAccount)
            {
                CExtKeyAccount *pSea;
                ExtKeyAccountMap::iterator it = pwallet->mapExtAccounts.find(id);
                if (it != pwallet->mapExtAccounts.end())
                {
                    pSea = it->second;
                } else
                if (wdb.ReadExtAccount(id, sea))
                {
                    pSea = &sea;
                } else
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Account not in wallet.");
                };

                if (0 != ManageExtAccount(*pSea, sOptName, sOptValue, result, sError))
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Error: " + sError);
                };

                if (sOptValue.length() > 0
                    && !wdb.WriteExtAccount(id, *pSea))
                {
                    wdb.TxnAbort();
                    throw std::runtime_error("Write failed.");
                };
            };

            if (sOptValue.length() == 0)
            {
                wdb.TxnAbort();
            } else
            {
                if (!wdb.TxnCommit())
                    throw std::runtime_error("TxnCommit failed.");
                result.pushKV("result", "Success.");
            };
        } // cs_wallet

    } else
    {
        throw std::runtime_error(help);
    };

    return result;
};

UniValue extkeyimportinternal(const JSONRPCRequest &request, bool fGenesisChain)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    EnsureWalletIsUnlocked(pwallet);

    if (request.params.size() < 1)
        throw std::runtime_error("Please specify a private extkey or mnemonic phrase.");

    std::string sMnemonic = request.params[0].get_str();
    bool fSaveBip44Root = false;
    std::string sLblMaster = "Master Key";
    std::string sLblAccount = "Default Account";
    std::string sPassphrase = "";
    std::string sError;
    int64_t nScanFrom = 1;

    if (request.params.size() > 1)
        sPassphrase = request.params[1].get_str();

    if (request.params.size() > 2)
    {
        std::string s = request.params[2].get_str();

        if (!nix::GetStringBool(s, fSaveBip44Root))
            throw std::runtime_error(strprintf("Unknown argument for save_bip44_root: %s.", s.c_str()));
    };

    if (request.params.size() > 3)
        sLblMaster = request.params[3].get_str();
    if (request.params.size() > 4)
        sLblAccount = request.params[4].get_str();

    if (request.params[5].isStr())
    {
        std::string s = request.params[5].get_str();
        if (!ParseInt64(s, &nScanFrom))
            throw std::runtime_error(strprintf("Unknown argument for scan_chain_from: %s.", s.c_str()));
    } else
    if (request.params[5].isNum())
    {
        nScanFrom = request.params[5].get_int64();
    };
    if (request.params.size() > 6)
        throw std::runtime_error(strprintf("Unknown parameter '%s'", request.params[6].get_str()));

    LogPrintf("Importing master key and account with labels '%s', '%s'.\n", sLblMaster.c_str(), sLblAccount.c_str());

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    CExtKey58 eKey58;
    CExtKeyPair ekp;
    if (eKey58.Set58(sMnemonic.c_str()) == 0)
    {
        if (!eKey58.IsValid(CChainParams::EXT_SECRET_KEY)
            && !eKey58.IsValid(CChainParams::EXT_SECRET_KEY_BTC))
            throw std::runtime_error("Please specify a private extkey or mnemonic phrase.");

        // Key was provided directly
        ekp = eKey58.GetKey();
    } else
    {
        std::vector<uint8_t> vSeed, vEntropy;

        // First check the mnemonic is valid
        if (0 != MnemonicDecode(-1, sMnemonic, vEntropy, sError))
            throw std::runtime_error(strprintf("MnemonicDecode failed: %s", sError.c_str()));

        if (0 != MnemonicToSeed(sMnemonic, sPassphrase, vSeed))
            throw std::runtime_error("MnemonicToSeed failed.");

        ekp.SetMaster(&vSeed[0], vSeed.size());
    };

    CStoredExtKey sek;
    sek.sLabel = sLblMaster;

    std::vector<uint8_t> v;
    sek.mapValue[EKVT_CREATED_AT] = SetCompressedInt64(v, GetTime());
    sek.kp = ekp;

    UniValue result(UniValue::VOBJ);

    int rv;
    bool fBip44 = true;
    CKeyID idDerived;
    CExtKeyAccount *sea;

    {
        LOCK(pwallet->cs_wallet);
        CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");
        if (!wdb.TxnBegin())
            throw std::runtime_error("TxnBegin failed.");

        if (0 != (rv = pwallet->ExtKeyImportLoose(&wdb, sek, idDerived, fBip44, fSaveBip44Root)))
        {
            wdb.TxnAbort();
            throw std::runtime_error(strprintf("ExtKeyImportLoose failed, %s", ExtKeyGetString(rv)));
        };

        if (0 != (rv = pwallet->ExtKeySetMaster(&wdb, idDerived)))
        {
            wdb.TxnAbort();
            throw std::runtime_error(strprintf("ExtKeySetMaster failed, %s.", ExtKeyGetString(rv)));
        };

        sea = new CExtKeyAccount();
        if (0 != (rv = pwallet->ExtKeyDeriveNewAccount(&wdb, sea, sLblAccount)))
        {
            pwallet->ExtKeyRemoveAccountFromMapsAndFree(sea);
            wdb.TxnAbort();
            throw std::runtime_error(strprintf("ExtKeyDeriveNewAccount failed, %s.", ExtKeyGetString(rv)));
        };

        CKeyID idNewDefaultAccount = sea->GetID();
        CKeyID idOldDefault = pwallet->idDefaultAccount;

        if (0 != (rv = pwallet->ExtKeySetDefaultAccount(&wdb, idNewDefaultAccount)))
        {
            pwallet->ExtKeyRemoveAccountFromMapsAndFree(sea);
            wdb.TxnAbort();
            throw std::runtime_error(strprintf("ExtKeySetDefaultAccount failed, %s.", ExtKeyGetString(rv)));
        };

        if (fGenesisChain)
        {
            std::string genesisChainLabel = "Genesis Import";
            CStoredExtKey *sekGenesisChain = new CStoredExtKey();

            if (0 != (rv = pwallet->NewExtKeyFromAccount(&wdb, idNewDefaultAccount,
                genesisChainLabel, sekGenesisChain, nullptr, &CHAIN_NO_GENESIS)))
            {
                delete sekGenesisChain;
                pwallet->ExtKeyRemoveAccountFromMapsAndFree(sea);
                wdb.TxnAbort();
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf(_("NewExtKeyFromAccount failed, %s."), ExtKeyGetString(rv)));
            };
        };

        if (!wdb.TxnCommit())
        {
            pwallet->idDefaultAccount = idOldDefault;
            pwallet->ExtKeyRemoveAccountFromMapsAndFree(sea);
            throw std::runtime_error("TxnCommit failed.");
        };
    } // cs_wallet

    pwallet->RescanFromTime(nScanFrom, reserver, true);
    pwallet->MarkDirty();
    pwallet->ReacceptWalletTransactions();

    UniValue warnings(UniValue::VARR);

    CBitcoinAddress addr;
    addr.Set(idDerived, CChainParams::EXT_KEY_HASH);
    result.pushKV("result", "Success.");
    result.pushKV("master_id", addr.ToString());
    result.pushKV("master_label", sek.sLabel);

    result.pushKV("account_id", sea->GetIDString58());
    result.pushKV("account_label", sea->sLabel);

    result.pushKV("note", "Please backup your wallet.");

    if (warnings.size() > 0)
        result.pushKV("warnings", warnings);

    return result;
}

UniValue extkeyimportmaster(const JSONRPCRequest &request)
{
    // Doesn't generate key, require users to run mnemonic new, more likely they'll save the phrase
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 6)
        throw std::runtime_error(
        "extkeyimportmaster \"mnemonic/key\" ( \"passphrase\" save_bip44_root master_label account_label scan_chain_from )\n"
        "Import master key from bip44 mnemonic root key and derive default account.\n"
        + HelpRequiringPassphrase(pwallet) +
        "\nArguments:\n"
        "1. \"mnemonic/key\"          (string, required) The mnemonic or root extended key.\n"
        "       Use '-stdin' to be prompted to enter a passphrase.\n"
        "       if mnemonic is blank, defaults to '-stdin'.\n"
        "2. \"passphrase\":           (string, optional) passphrase when importing mnemonic - default blank.\n"
        "       Use '-stdin' to be prompted to enter a passphrase.\n"
        "3. save_bip44_root:        (bool, optional) Save bip44 root key to wallet - default false.\n"
        "4. \"master_label\":         (string, optional) Label for master key - default 'Master Key'.\n"
        "5. \"account_label\":        (string, optional) Label for account - default 'Default Account'.\n"
        "6. scan_chain_from:        (int, optional) Scan for transactions in blocks after timestamp - default 1.\n"
        "\nExamples:\n"
        + HelpExampleCli("extkeyimportmaster", "-stdin -stdin false \"label_master\" \"label_account\"")
        + HelpExampleCli("extkeyimportmaster", "\"word1 ... word24\" \"passphrase\" false \"label_master\" \"label_account\"")
        + HelpExampleRpc("extkeyimportmaster", "\"word1 ... word24\", \"passphrase\", false, \"label_master\", \"label_account\""));

    ObserveSafeMode();

    return extkeyimportinternal(request, false);
};

UniValue extkeygenesisimport(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 6)
        throw std::runtime_error(
        "extkeygenesisimport \"mnemonic/key\" ( \"passphrase\" save_bip44_root master_label account_label scan_chain_from )\n"
        "Import master key from bip44 mnemonic root key and derive default account.\n"
        "Derives an extra chain from path 444444 to receive imported coin.\n"
        + HelpRequiringPassphrase(pwallet) +
        "\nArguments:\n"
        "1. \"mnemonic/key\"          (string, required) The mnemonic or root extended key.\n"
        "       Use '-stdin' to be prompted to enter a passphrase.\n"
        "       if mnemonic is blank, defaults to '-stdin'.\n"
        "2. \"passphrase\":           (string, optional) passphrase when importing mnemonic - default blank.\n"
        "       Use '-stdin' to be prompted to enter a passphrase.\n"
        "3. save_bip44_root:        (bool, optional) Save bip44 root key to wallet - default false.\n"
        "4. \"master_label\":         (string, optional) Label for master key - default 'Master Key'.\n"
        "5. \"account_label\":        (string, optional) Label for account - default 'Default Account'.\n"
        "6. scan_chain_from:        (int, optional) Scan for transactions in blocks after timestamp - default 1.\n"
        "\nExamples:\n"
        + HelpExampleCli("extkeygenesisimport", "-stdin -stdin false \"label_master\" \"label_account\"")
        + HelpExampleCli("extkeygenesisimport", "\"word1 ... word24\" \"passphrase\" false \"label_master\" \"label_account\"")
        + HelpExampleRpc("extkeygenesisimport", "\"word1 ... word24\", \"passphrase\", false, \"label_master\", \"label_account\""));

    ObserveSafeMode();

    return extkeyimportinternal(request, true);
}

UniValue extkeyaltversion(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "extkeyaltversion \"ext_key\"\n"
            "Returns the provided ext_key encoded with alternate version bytes.\n"
            "If the provided ext_key has a Bitcoin prefix the output will be encoded with a NIX prefix.\n"
            "If the provided ext_key has a NIX prefix the output will be encoded with a Bitcoin prefix.");

    ObserveSafeMode();

    std::string sKeyIn = request.params[0].get_str();
    std::string sKeyOut;

    CExtKey58 eKey58;
    CExtKeyPair ekp;
    if (eKey58.Set58(sKeyIn.c_str()) != 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Invalid input key."));

    // TODO: handle testnet keys on main etc
    if (eKey58.IsValid(CChainParams::EXT_SECRET_KEY_BTC))
        return eKey58.ToStringVersion(CChainParams::EXT_SECRET_KEY);
    if (eKey58.IsValid(CChainParams::EXT_SECRET_KEY))
        return eKey58.ToStringVersion(CChainParams::EXT_SECRET_KEY_BTC);

    if (eKey58.IsValid(CChainParams::EXT_PUBLIC_KEY_BTC))
        return eKey58.ToStringVersion(CChainParams::EXT_PUBLIC_KEY);
    if (eKey58.IsValid(CChainParams::EXT_PUBLIC_KEY))
        return eKey58.ToStringVersion(CChainParams::EXT_PUBLIC_KEY_BTC);

    throw JSONRPCError(RPC_INVALID_PARAMETER, _("Unknown input key version."));
}


UniValue getnewextaddress(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "getnewextaddress ( \"label\" childNo bech32 hardened )\n"
            "Returns a new NIX ext address for receiving payments.\n"
            + HelpRequiringPassphrase(pwallet) +
            "\nArguments:\n"
            "1. \"label\"             (string, optional) If specified the key is added to the address book.\n"
            "2. \"childNo\"           (string, optional), If specified the account derive counter is not updated.\n"
            "3. bech32              (bool, optional, default=false) Use Bech32 encoding.\n"
            "4. hardened            (bool, optional, default=false) Derive a hardened key.\n"
            "\nResult:\n"
            "\"address\"              (string) The new NIX extended address\n"
            "\nExamples:\n"
            + HelpExampleCli("getnewextaddress", "")
            + HelpExampleRpc("getnewextaddress", ""));

    EnsureWalletIsUnlocked(pwallet);

    uint32_t nChild = 0;
    uint32_t *pChild = nullptr;
    std::string strLabel;
    const char *pLabel = nullptr;
    if (request.params[0].isStr())
    {
        strLabel = request.params[0].get_str();
        if (strLabel.size() > 0)
            pLabel = strLabel.c_str();
    };

    if (request.params[1].isStr())
    {
        std::string s = request.params[1].get_str();
        if (!s.empty())
        {
            // TODO, make full path work
            std::vector<uint32_t> vPath;
            if (0 != ExtractExtKeyPath(s, vPath) || vPath.size() != 1)
                throw JSONRPCError(RPC_INVALID_PARAMETER, _("bad childNo."));
            nChild = vPath[0];
            pChild = &nChild;
        };
    };

    bool fBech32 = !request.params[2].isNull() ? request.params[2].get_bool() : false;
    bool fHardened = !request.params[3].isNull() ? request.params[3].get_bool() : false;

    CStoredExtKey *sek = new CStoredExtKey();
    if (0 != pwallet->NewExtKeyFromAccount(strLabel, sek, pLabel, pChild, fHardened, fBech32))
    {
        delete sek;
        throw JSONRPCError(RPC_WALLET_ERROR, _("NewExtKeyFromAccount failed."));
    };

    // CBitcoinAddress displays public key only
    return CBitcoinAddress(sek->kp, fBech32).ToString();
}

UniValue getnewstealthaddress(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 5)
        throw std::runtime_error(
            "getnewstealthaddress ( \"label\" num_prefix_bits prefix_num bech32 makeV2 )\n"
            "Returns a new NIX stealth address for receiving payments."
            + HelpRequiringPassphrase(pwallet) +
            "\nArguments:\n"
            "1. \"label\"             (string, optional) If specified the key is added to the address book.\n"
            "2. num_prefix_bits     (int, optional) If specified and > 0, the stealth address is created with a prefix.\n"
            "3. prefix_num          (int, optional) If prefix_num is not specified the prefix will be selected deterministically.\n"
            "           prefix_num can be specified in base2, 10 or 16, for base 2 prefix_num must begin with 0b, 0x for base16.\n"
            "           A 32bit integer will be created from prefix_num and the least significant num_prefix_bits will become the prefix.\n"
            "           A stealth address created without a prefix will scan all incoming stealth transactions, irrespective of transaction prefixes.\n"
            "           Stealth addresses with prefixes will scan only incoming stealth transactions with a matching prefix.\n"
            "4. bech32              (bool, optional, default=false) Use Bech32 encoding.\n"
            "5. makeV2              (bool, optional, default=false) Generate an address from the same method used for hardware wallets.\n"
            "\nResult:\n"
            "\"address\"              (string) The new NIX stealth address\n"
            "\nExamples:\n"
            + HelpExampleCli("getnewstealthaddress", "\"lblTestSxAddrPrefix\" 3 \"0b101\"")
            + HelpExampleRpc("getnewstealthaddress", "\"lblTestSxAddrPrefix\", 3, \"0b101\""));

    EnsureWalletIsUnlocked(pwallet);

    std::string sLabel;
    if (request.params.size() > 0)
        sLabel = request.params[0].get_str();

    uint32_t num_prefix_bits = 0;
    if (request.params.size() > 1)
    {
        std::string sTemp = request.params[1].get_str();
        char *pend;
        errno = 0;
        num_prefix_bits = strtoul(sTemp.c_str(), &pend, 10);
        if (errno != 0 || !pend || *pend != '\0')
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("num_prefix_bits invalid number."));
    };

    if (num_prefix_bits > 32)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("num_prefix_bits must be <= 32."));

    std::string sPrefix_num;
    if (request.params.size() > 2)
        sPrefix_num = request.params[2].get_str();

    bool fBech32 = request.params.size() > 3 ? request.params[3].get_bool() : false;
    bool fMakeV2 = request.params.size() > 4 ? request.params[4].get_bool() : false;

    if (fMakeV2 && !fBech32)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("bech32 must be true when using makeV2."));

    CEKAStealthKey akStealth;
    std::string sError;
    if (fMakeV2)
    {
        if (0 != pwallet->NewStealthKeyV2FromAccount(sLabel, akStealth, num_prefix_bits, sPrefix_num.empty() ? nullptr : sPrefix_num.c_str(), fBech32))
            throw JSONRPCError(RPC_WALLET_ERROR, _("NewStealthKeyV2FromAccount failed."));
    } else
    {
        if (0 != pwallet->NewStealthKeyFromAccount(sLabel, akStealth, num_prefix_bits, sPrefix_num.empty() ? nullptr : sPrefix_num.c_str(), fBech32))
            throw JSONRPCError(RPC_WALLET_ERROR, _("NewStealthKeyFromAccount failed."));
    };

    CStealthAddress sxAddr;
    akStealth.SetSxAddr(sxAddr);

    return sxAddr.ToString(fBech32);
}

UniValue importstealthaddress(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 6)
        throw std::runtime_error(
            "importstealthaddress \"scan_secret\" \"spend_secret\" ( \"label\" num_prefix_bits prefix_num bech32 )\n"
            "Import an owned stealth addresses.\n"
            + HelpRequiringPassphrase(pwallet) +
            "\nArguments:\n"
            "1. \"scan_secret\"       (string, required) The hex or wif encoded scan secret.\n"
            "2. \"spend_secret\"      (string, required) The hex or wif encoded spend secret.\n"
            "3. \"label\"             (string, optional) If specified the key is added to the address book.\n"
            "4. num_prefix_bits     (int, optional) If specified and > 0, the stealth address is created with a prefix.\n"
            "5. prefix_num          (int, optional) If prefix_num is not specified the prefix will be selected deterministically.\n"
            "           prefix_num can be specified in base2, 10 or 16, for base 2 prefix_num must begin with 0b, 0x for base16.\n"
            "           A 32bit integer will be created from prefix_num and the least significant num_prefix_bits will become the prefix.\n"
            "           A stealth address created without a prefix will scan all incoming stealth transactions, irrespective of transaction prefixes.\n"
            "           Stealth addresses with prefixes will scan only incoming stealth transactions with a matching prefix.\n"
            "6. bech32              (bool, optional) Use Bech32 encoding.\n"
            "\nResult:\n"
            "\"address\"              (string) The new NIX stealth address\n"
            "\nExamples:\n"
            + HelpExampleCli("importstealthaddress", "scan_secret spend_secret \"label\" 3 \"0b101\"")
            + HelpExampleRpc("importstealthaddress", "scan_secret, spend_secret, \"label\", 3, \"0b101\""));

    EnsureWalletIsUnlocked(pwallet);

    std::string sScanSecret  = request.params[0].get_str();
    std::string sSpendSecret = request.params[1].get_str();
    std::string sLabel;

    if (request.params.size() > 2)
        sLabel = request.params[2].get_str();

    uint32_t num_prefix_bits = 0;
    if (request.params.size() > 3)
    {
        std::string sTemp = request.params[3].get_str();
        char *pend;
        errno = 0;
        num_prefix_bits = strtoul(sTemp.c_str(), &pend, 10);
        if (errno != 0 || !pend || *pend != '\0')
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("num_prefix_bits invalid number."));
    };

    if (num_prefix_bits > 32)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("num_prefix_bits must be <= 32."));

    uint32_t nPrefix = 0;
    std::string sPrefix_num;
    if (request.params.size() > 4)
    {
        sPrefix_num = request.params[4].get_str();
        if (!ExtractStealthPrefix(sPrefix_num.c_str(), nPrefix))
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Could not convert prefix to number."));
    };

    bool fBech32 = request.params.size() > 5 ? request.params[5].get_bool() : false;

    std::vector<uint8_t> vchScanSecret;
    std::vector<uint8_t> vchSpendSecret;
    CBitcoinSecret wifScanSecret, wifSpendSecret;
    CKey skScan, skSpend;
    if (IsHex(sScanSecret))
    {
        vchScanSecret = ParseHex(sScanSecret);
    } else
    if (wifScanSecret.SetString(sScanSecret))
    {
        skScan = wifScanSecret.GetKey();
    } else
    {
        if (!DecodeBase58(sScanSecret, vchScanSecret))
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Could not decode scan secret as wif, hex or base58."));
    };
    if (vchScanSecret.size() > 0)
    {
        if (vchScanSecret.size() != 32)
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Scan secret is not 32 bytes."));
        skScan.Set(&vchScanSecret[0], true);
    };

    if (IsHex(sSpendSecret))
    {
        vchSpendSecret = ParseHex(sSpendSecret);
    } else
    if (wifSpendSecret.SetString(sSpendSecret))
    {
        skSpend = wifSpendSecret.GetKey();
    } else
    {
        if (!DecodeBase58(sSpendSecret, vchSpendSecret))
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Could not decode spend secret as hex or base58."));
    };
    if (vchSpendSecret.size() > 0)
    {
        if (vchSpendSecret.size() != 32)
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Spend secret is not 32 bytes."));
        skSpend.Set(&vchSpendSecret[0], true);
    };

    if (skSpend == skScan)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Spend secret must be different to scan secret."));

    CStealthAddress sxAddr;
    sxAddr.label = sLabel;
    sxAddr.scan_secret = skScan;
    sxAddr.spend_secret_id = skSpend.GetPubKey().GetID();

    sxAddr.prefix.number_bits = num_prefix_bits;
    if (sxAddr.prefix.number_bits > 0)
    {
        if (sPrefix_num.empty())
        {
            // if pPrefix is null, set nPrefix from the hash of kSpend
            uint8_t tmp32[32];
            CSHA256().Write(skSpend.begin(), 32).Finalize(tmp32);
            memcpy(&nPrefix, tmp32, 4);
        };

        uint32_t nMask = SetStealthMask(num_prefix_bits);
        nPrefix = nPrefix & nMask;
        sxAddr.prefix.bitfield = nPrefix;
    };

    if (0 != SecretToPublicKey(sxAddr.scan_secret, sxAddr.scan_pubkey))
        throw JSONRPCError(RPC_INTERNAL_ERROR, _("Could not get scan public key."));
    if (0 != SecretToPublicKey(skSpend, sxAddr.spend_pubkey))
        throw JSONRPCError(RPC_INTERNAL_ERROR, _("Could not get spend public key."));

    UniValue result(UniValue::VOBJ);
    bool fFound = false;
    // Find if address already exists, can update
    std::set<CStealthAddress>::iterator it;
    for (it = pwallet->stealthAddresses.begin(); it != pwallet->stealthAddresses.end(); ++it)
    {
        CStealthAddress &sxAddrIt = const_cast<CStealthAddress&>(*it);
        if (sxAddrIt.scan_pubkey == sxAddr.scan_pubkey
            && sxAddrIt.spend_pubkey == sxAddr.spend_pubkey)
        {
            CKeyID sid = sxAddrIt.GetSpendKeyID();

            if (!pwallet->HaveKey(sid))
            {
                CPubKey pk = skSpend.GetPubKey();
                if (!pwallet->AddKeyPubKey(skSpend, pk))
                    throw JSONRPCError(RPC_WALLET_ERROR, _("Import failed - AddKeyPubKey failed."));
                fFound = true; // update stealth address with secret
                break;
            };

            throw JSONRPCError(RPC_WALLET_ERROR, _("Import failed - stealth address exists."));
        };
    };

    {
        LOCK(pwallet->cs_wallet);
        if (pwallet->HaveStealthAddress(sxAddr)) // check for extkeys, no update possible
            throw JSONRPCError(RPC_WALLET_ERROR, _("Import failed - stealth address exists."));

        pwallet->SetAddressBook(sxAddr, sLabel, "", fBech32);
    }

    if (fFound)
    {
        result.pushKV("result", "Success, updated " + sxAddr.Encoded(fBech32));
    } else
    {
        if (!pwallet->ImportStealthAddress(sxAddr, skSpend))
            throw std::runtime_error("Could not save to wallet.");
        result.pushKV("result", "Success");
        result.pushKV("stealth_address", sxAddr.Encoded(fBech32));
    };

    return result;
}

int ListLooseStealthAddresses(UniValue &arr, CHDWallet *pwallet, bool fShowSecrets, bool fAddressBookInfo)
{
    std::set<CStealthAddress>::iterator it;
    for (it = pwallet->stealthAddresses.begin(); it != pwallet->stealthAddresses.end(); ++it)
    {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("Label", it->label);
        obj.pushKV("Address", it->Encoded());

        if (fShowSecrets)
        {
            obj.pushKV("Scan Secret", CBitcoinSecret(it->scan_secret).ToString());

            CKeyID sid = it->GetSpendKeyID();
            CKey skSpend;
            if (pwallet->GetKey(sid, skSpend))
                obj.pushKV("Spend Secret", CBitcoinSecret(skSpend).ToString());
        };

        if (fAddressBookInfo)
        {
            std::map<CTxDestination, CAddressBookData>::const_iterator mi = pwallet->mapAddressBook.find(*it);
            if (mi != pwallet->mapAddressBook.end())
            {
                // TODO: confirm vPath?

                if (mi->second.name != it->label)
                    obj.pushKV("addr_book_label", mi->second.name);
                if (!mi->second.purpose.empty())
                    obj.pushKV("purpose", mi->second.purpose);

                UniValue objDestData(UniValue::VOBJ);
                for (const auto &pair : mi->second.destdata)
                    obj.pushKV(pair.first, pair.second);
                if (objDestData.size() > 0)
                    obj.pushKV("destdata", objDestData);
            };
        };

        arr.push_back(obj);
    };

    return 0;
};

UniValue liststealthaddresses(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "liststealthaddresses ( show_secrets=0 )\n"
            "List owned stealth addresses.");

    bool fShowSecrets = false;

    if (request.params.size() > 0)
    {
        std::string str = request.params[0].get_str();

        if (nix::IsStringBoolNegative(str))
            fShowSecrets = false;
        else
            fShowSecrets = true;
    };

    if (fShowSecrets)
        EnsureWalletIsUnlocked(pwallet);

    UniValue result(UniValue::VARR);

    ExtKeyAccountMap::const_iterator mi;
    for (mi = pwallet->mapExtAccounts.begin(); mi != pwallet->mapExtAccounts.end(); ++mi)
    {
        CExtKeyAccount *ea = mi->second;

        if (ea->mapStealthKeys.size() < 1)
            continue;

        UniValue rAcc(UniValue::VOBJ);
        UniValue arrayKeys(UniValue::VARR);

        rAcc.pushKV("Account", ea->sLabel);

        AccStealthKeyMap::iterator it;
        for (it = ea->mapStealthKeys.begin(); it != ea->mapStealthKeys.end(); ++it)
        {
            const CEKAStealthKey &aks = it->second;

            UniValue objA(UniValue::VOBJ);
            objA.pushKV("Label", aks.sLabel);
            objA.pushKV("Address", aks.ToStealthAddress());

            if (fShowSecrets)
            {
                objA.pushKV("Scan Secret", HexStr(aks.skScan.begin(), aks.skScan.end()));
                std::string sSpend;
                CStoredExtKey *sekAccount = ea->ChainAccount();
                if (sekAccount && !sekAccount->fLocked)
                {
                    CKey skSpend;
                    if (ea->GetKey(aks.akSpend, skSpend))
                        sSpend = HexStr(skSpend.begin(), skSpend.end());
                    else
                        sSpend = "Extract failed.";
                } else
                {
                    sSpend = "Account Locked.";
                };
                objA.pushKV("Spend Secret", sSpend);
            };

            arrayKeys.push_back(objA);
        };

        if (arrayKeys.size() > 0)
        {
            rAcc.pushKV("Stealth Addresses", arrayKeys);
            result.push_back(rAcc);
        };
    };


    if (pwallet->stealthAddresses.size() > 0)
    {
        UniValue rAcc(UniValue::VOBJ);
        UniValue arrayKeys(UniValue::VARR);

        rAcc.pushKV("Account", "Loose Keys");

        ListLooseStealthAddresses(arrayKeys, pwallet, fShowSecrets, false);

        if (arrayKeys.size() > 0)
        {
            rAcc.pushKV("Stealth Addresses", arrayKeys);
            result.push_back(rAcc);
        };
    };

    return result;
}


UniValue scanchain(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "scanchain [from_height]\n"
            "\nDEPRECATED, will be removed in 0.17. Replaced by rescanblockchain.\n"
            "Scan blockchain for owned transactions.");

    //EnsureWalletIsUnlocked(pwallet);

    UniValue result(UniValue::VOBJ);
    int32_t nFromHeight = 0;

    if (request.params.size() > 0)
        nFromHeight = request.params[0].get_int();

    pwallet->ScanChainFromHeight(nFromHeight);

    result.pushKV("result", "Scan complete.");

    return result;
}

UniValue reservebalance(const JSONRPCRequest &request)
{
    // Reserve balance from being staked for network protection

    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "reservebalance reserve ( amount )\n"
            "reserve is true or false to turn balance reserve on or off.\n"
            "amount is a real and rounded to cent.\n"
            "Set reserve amount not participating in network protection.\n"
            "If no parameters provided current setting is printed.\n"
            "Wallet must be unlocked to modify.\n");

    if (request.params.size() > 0)
    {
        EnsureWalletIsUnlocked(pwallet);

        bool fReserve = request.params[0].get_bool();
        if (fReserve)
        {
            if (request.params.size() == 1)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "must provide amount to reserve balance.");
            int64_t nAmount = AmountFromValue(request.params[1]);
            nAmount = (nAmount / CENT) * CENT;  // round to cent
            if (nAmount < 0)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "amount cannot be negative.");
            pwallet->SetReserveBalance(nAmount);
        } else
        {
            if (request.params.size() > 1)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "cannot specify amount to turn off reserve.");
            pwallet->SetReserveBalance(0);
        };
    };

    UniValue result(UniValue::VOBJ);
    result.pushKV("reserve", (pwallet->nReserveBalance > 0));
    result.pushKV("amount", ValueFromAmount(pwallet->nReserveBalance));
    return result;
}

UniValue deriverangekeys(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 ||request.params.size() > 7)
        throw std::runtime_error(
            "deriverangekeys start ( end \"key/id\" hardened save add_to_addressbook 256bithash )\n"
            "Derive keys from the specified chain.\n"
            "Wallet must be unlocked if save or hardened options are set.\n"
            "\nArguments:\n"
            "1. start               (int, required) Start from key.\n"
            "2. end                 (int, optional) Stop deriving after key, default set to derive one key.\n"
            "3. \"key/id\"            (string, optional)  Account to derive from, default external chain of current account.\n"
            "4. hardened            (bool, optional, default=false) Derive hardened keys.\n"
            "5. save                (bool, optional, default=false) Save derived keys to the wallet.\n"
            "6. add_to_addressbook  (bool, optional, default=false) Add derived keys to address book, only applies when saving keys.\n"
            "7. 256bithash          (bool, optional, default=false) Display addresses from sha256 hash of public keys.\n"
            "\nResult:\n"
            "\"addresses\"            (json) Array of derived addresses\n"
            "\nExamples:\n"
            + HelpExampleCli("deriverangekeys", "0 1")
            + HelpExampleRpc("deriverangekeys", "0, 1"));

    ObserveSafeMode();

    // TODO: manage nGenerated, nHGenerated properly

    int nStart = request.params[0].get_int();
    int nEnd = nStart;

    if (request.params.size() > 1)
        nEnd = request.params[1].get_int();

    if (nEnd < nStart)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "end can not be before start.");

    if (nStart < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "start can not be negative.");

    if (nEnd < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "end can not be positive.");

    std::string sInKey;
    if (request.params.size() > 2)
        sInKey = request.params[2].get_str();

    bool fHardened = false;
    if (request.params.size() > 3)
    {
        std::string s = request.params[3].get_str();
        if (!nix::GetStringBool(s, fHardened))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown argument for hardened: %s.", s.c_str()));
    };

    bool fSave = false;
    if (request.params.size() > 4)
    {
        std::string s = request.params[4].get_str();
        if (!nix::GetStringBool(s, fSave))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown argument for save: %s.", s.c_str()));
    };

    bool fAddToAddressBook = false;
    if (request.params.size() > 5)
    {
        std::string s = request.params[5].get_str();
        if (!nix::GetStringBool(s, fAddToAddressBook))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Unknown argument for add_to_addressbook: %s."), s.c_str()));
    };

    bool f256bit = false;
    if (request.params.size() > 6)
    {
        std::string s = request.params[6].get_str();
        if (!nix::GetStringBool(s, f256bit))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Unknown argument for 256bithash: %s."), s.c_str()));
    };

    if (!fSave && fAddToAddressBook)
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("add_to_addressbook can't be set without save"));

    if (fSave || fHardened)
        EnsureWalletIsUnlocked(pwallet);

    UniValue result(UniValue::VARR);

    {
        LOCK2(cs_main, pwallet->cs_wallet);

        CStoredExtKey *sek = nullptr;
        CExtKeyAccount *sea = nullptr;
        uint32_t nChain = 0;
        if (sInKey.length() == 0)
        {
            if (pwallet->idDefaultAccount.IsNull())
                throw JSONRPCError(RPC_WALLET_ERROR, _("No default account set."));

            ExtKeyAccountMap::iterator mi = pwallet->mapExtAccounts.find(pwallet->idDefaultAccount);
            if (mi == pwallet->mapExtAccounts.end())
                throw JSONRPCError(RPC_WALLET_ERROR, _("Unknown account."));

            sea = mi->second;
            nChain = sea->nActiveExternal;
            if (nChain < sea->vExtKeys.size())
                sek = sea->vExtKeys[nChain];
        } else
        {
            CKeyID keyId;
            ExtractExtKeyId(sInKey, keyId, CChainParams::EXT_KEY_HASH);

            ExtKeyAccountMap::iterator mi = pwallet->mapExtAccounts.begin();
            for (; mi != pwallet->mapExtAccounts.end(); ++mi)
            {
                sea = mi->second;
                for (uint32_t i = 0; i < sea->vExtKeyIDs.size(); ++i)
                {
                    if (sea->vExtKeyIDs[i] != keyId)
                        continue;
                    nChain = i;
                    sek = sea->vExtKeys[i];
                };
                if (sek)
                    break;
            };
        };

        CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");
        CStoredExtKey sekLoose, sekDB;
        if (!sek)
        {
            CExtKey58 eKey58;
            CBitcoinAddress addr;
            CKeyID idk;

            if (addr.SetString(sInKey)
                && addr.IsValid(CChainParams::EXT_KEY_HASH)
                && addr.GetKeyID(idk, CChainParams::EXT_KEY_HASH))
            {
                // idk is set
            } else
            if (eKey58.Set58(sInKey.c_str()) == 0)
            {
                sek = &sekLoose;
                sek->kp = eKey58.GetKey();
                idk = sek->kp.GetID();
            } else
            {
                throw JSONRPCError(RPC_WALLET_ERROR, _("Invalid key."));
            };

            if (!idk.IsNull())
            {
                if (wdb.ReadExtKey(idk, sekDB))
                {
                    sek = &sekDB;
                    if (fHardened && (sek->nFlags & EAF_IS_CRYPTED))
                        throw std::runtime_error("TODO: decrypt key.");
                };
            };
        };

        if (!sek)
            throw JSONRPCError(RPC_WALLET_ERROR, _("Unknown chain."));

        if (fHardened && !sek->kp.IsValidV())
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("extkey must have private key to derive hardened keys."));

        if (fSave && !sea)
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Must have account to save keys."));


        uint32_t idIndex;
        if (fAddToAddressBook)
        {
            if (0 != pwallet->ExtKeyGetIndex(sea, idIndex))
                throw JSONRPCError(RPC_WALLET_ERROR, _("ExtKeyGetIndex failed."));
        };

        uint32_t nChildIn = (uint32_t)nStart;
        CPubKey newKey;
        for (int i = nStart; i <= nEnd; ++i)
        {
            nChildIn = (uint32_t)i;
            uint32_t nChildOut = 0;
            if (0 != sek->DeriveKey(newKey, nChildIn, nChildOut, fHardened))
                throw JSONRPCError(RPC_WALLET_ERROR, "DeriveKey failed.");

            if (nChildIn != nChildOut)
                LogPrintf("Warning: %s - DeriveKey skipped key %d.\n", __func__, nChildIn);

            if (fHardened)
                SetHardenedBit(nChildOut);


            CKeyID idk = newKey.GetID();
            CKeyID256 idk256;
            if (f256bit)
            {
                idk256 = newKey.GetID256();
                result.push_back(CBitcoinAddress(idk256).ToString());
            } else
            {
                result.push_back(CBitcoinAddress(idk).ToString());
            };

            if (fSave)
            {
                if (HK_YES != sea->HaveSavedKey(idk))
                {
                    CEKAKey ak(nChain, nChildOut);
                    if (0 != pwallet->ExtKeySaveKey(sea, idk, ak))
                        throw JSONRPCError(RPC_WALLET_ERROR, "ExtKeySaveKey failed.");
                };

                if (fAddToAddressBook)
                {
                    std::vector<uint32_t> vPath;
                    vPath.push_back(idIndex); // first entry is the index to the account / master key

                    if (0 == AppendChainPath(sek, vPath))
                        vPath.push_back(nChildOut);
                    else
                        vPath.clear();

                    std::string strAccount = "";
                    if (f256bit)
                        pwallet->SetAddressBook(&wdb, idk256, strAccount, "receive", vPath, false);
                    else
                        pwallet->SetAddressBook(&wdb, idk, strAccount, "receive", vPath, false);
                };
            };
        };
    }

    return result;
}

UniValue clearwallettransactions(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "clearwallettransactions ( remove_all )\n"
            "Delete transactions from the wallet.\n"
            "Warning: Backup your wallet before using!\n"
            + HelpRequiringPassphrase(pwallet) +
            "\nArguments:\n"
            "1. remove_all           (bool, optional, default=false) Remove all transactions.\n"
            "\nExamples:\n"
            + HelpExampleCli("clearwallettransactions", "")
            + HelpExampleRpc("clearwallettransactions", "true"));

    ObserveSafeMode();

    EnsureWalletIsUnlocked(pwallet);

    bool fRemoveAll = false;

    if(request.params.size() > 0){
        std::string req;
        if(request.params[0].isBool())
            fRemoveAll = request.params[0].getBool();
        else{
            req = request.params[0].get_str();
            if (!nix::GetStringBool(req, fRemoveAll))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Not a bool value.");
        }
    }

    int rv;
    size_t nRemoved = 0;
    size_t nRecordsRemoved = 0;

    {
        LOCK2(cs_main, pwallet->cs_wallet);

        CHDWalletDB wdb(pwallet->GetDBHandle());
        if (!wdb.TxnBegin())
            throw std::runtime_error("TxnBegin failed.");

        Dbc *pcursor = wdb.GetTxnCursor();
        if (!pcursor)
            throw std::runtime_error("GetTxnCursor failed.");

        CDataStream ssKey(SER_DISK, CLIENT_VERSION);

        std::map<uint256, CWalletTx>::iterator itw;
        std::string strType;
        uint256 hash;
        uint32_t fFlags = DB_SET_RANGE;
        ssKey << std::string("tx");
        while (wdb.ReadKeyAtCursor(pcursor, ssKey, fFlags) == 0)
        {
            fFlags = DB_NEXT;

            ssKey >> strType;
            if (strType != "tx")
                break;
            ssKey >> hash;

            if (!fRemoveAll)
            {
                if ((itw = pwallet->mapWallet.find(hash)) == pwallet->mapWallet.end())
                {
                    LogPrintf("Warning: %s - tx not found in mapwallet! %s.\n", __func__, hash.ToString());
                    continue; // err on the side of caution
                };

                CWalletTx *pcoin = &itw->second;
                if (!pcoin->isAbandoned())
                    continue;
            };

            //if (0 != pwallet->UnloadTransaction(hash))
            //    throw std::runtime_error("UnloadTransaction failed.");
            pwallet->UnloadTransaction(hash); // ignore failure

            if ((rv = pcursor->del(0)) != 0)
                throw std::runtime_error("pcursor->del failed.");

            nRemoved++;
        };

        if (fRemoveAll)
        {
            fFlags = DB_SET_RANGE;
            ssKey.clear();
            ssKey << std::string("rtx");
            while (wdb.ReadKeyAtCursor(pcursor, ssKey, fFlags) == 0)
            {
                fFlags = DB_NEXT;

                ssKey >> strType;
                if (strType != "rtx")
                    break;
                ssKey >> hash;

                pwallet->UnloadTransaction(hash); // ignore failure

                if ((rv = pcursor->del(0)) != 0)
                    throw std::runtime_error("pcursor->del failed.");

                // TODO: Remove CStoredTransaction

                nRecordsRemoved++;
            };
        };

        pcursor->close();
        if (!wdb.TxnCommit())
        {
            throw std::runtime_error("TxnCommit failed.");
        };
    }

    UniValue result(UniValue::VOBJ);

    result.pushKV("transactions_removed", (int)nRemoved);
    result.pushKV("records_removed", (int)nRecordsRemoved);

    return result;
}

static bool ParseOutput(
    UniValue &                 output,
    const COutputEntry &       o,
    CHDWallet * const          pwallet,
    CWalletTx &                wtx,
    const isminefilter &       watchonly,
    std::vector<std::string> & addresses,
    std::vector<std::string> & amounts
) {
    CBitcoinAddress addr;

    std::string sKey = strprintf("n%d", o.vout);
    mapValue_t::iterator mvi = wtx.mapValue.find(sKey);
    if (mvi != wtx.mapValue.end()) {
        output.pushKV("narration", mvi->second);
    }
    if (addr.Set(o.destination)) {
        output.pushKV("address", addr.ToString());
        addresses.push_back(addr.ToString());
    }
    if (o.ismine & ISMINE_WATCH_ONLY) {
        if (watchonly & ISMINE_WATCH_ONLY) {
            output.pushKV("involvesWatchonly", true);
        } else {
            return false;
        }
    }
    if (pwallet->mapAddressBook.count(o.destination)) {
        output.pushKV("label", pwallet->mapAddressBook[o.destination].name);
    }
    output.pushKV("vout", o.vout);
    amounts.push_back(std::to_string(o.amount));
    return true;
}

static void ParseOutputs(
    UniValue &           entries,
    CWalletTx &          wtx,
    CHDWallet * const    pwallet,
    const isminefilter & watchonly,
    std::string          search,
    bool                 fWithReward,
    bool                 fBech32
) {
    UniValue entry(UniValue::VOBJ);

    // GetAmounts variables
    std::list<COutputEntry> listReceived;
    std::list<COutputEntry> listSent;
    CAmount nFee;
    CAmount amount = 0;
    std::string strSentAccount;

    wtx.GetAmounts(
        listReceived,
        listSent,
        nFee,
        strSentAccount,
        ISMINE_ALL);

    if (wtx.IsFromMe(ISMINE_WATCH_ONLY) && !(watchonly & ISMINE_WATCH_ONLY)) {
        return ;
    }

    std::vector<std::string> addresses;
    std::vector<std::string> amounts;

    UniValue outputs(UniValue::VARR);
    // common to every type of transaction
    if (strSentAccount != "") {
        entry.pushKV("account", strSentAccount);
    }
    WalletTxToJSON(wtx, entry, true);

    if (!listSent.empty())
        entry.pushKV("abandoned", wtx.isAbandoned());

    {
        // sent
        if (!listSent.empty()) {
            entry.pushKV("fee", ValueFromAmount(-nFee));
            for (const auto &s : listSent) {
                UniValue output(UniValue::VOBJ);
                if (!ParseOutput(output,
                    s,
                    pwallet,
                    wtx,
                    watchonly,
                    addresses,
                    amounts
                )) {
                    return ;
                }
                output.pushKV("amount", ValueFromAmount(-s.amount));
                amount -= s.amount;
                outputs.push_back(output);
            }
        }

        // received
        if (!listReceived.empty()) {
            for (const auto &r : listReceived) {
                UniValue output(UniValue::VOBJ);
                if (!ParseOutput(
                    output,
                    r,
                    pwallet,
                    wtx,
                    watchonly,
                    addresses,
                    amounts
                )) {
                    return ;
                }
                if (r.destination.type() == typeid(CKeyID)) {
                    CStealthAddress sx;
                    CKeyID idK = boost::get<CKeyID>(r.destination);
                    if (pwallet->GetStealthLinked(idK, sx)) {
                        output.pushKV("stealth_address", sx.Encoded(fBech32));
                    }
                }
                output.pushKV("amount", ValueFromAmount(r.amount));
                amount += r.amount;

                bool fExists = false;
                for (size_t i = 0; i < outputs.size(); ++i) {
                    auto &o = outputs.get(i);
                    if (o["vout"].get_int() == r.vout) {
                        o.get("amount").setStr(nix::AmountToString(r.amount));
                        fExists = true;
                    }
                }
                if (!fExists)
                    outputs.push_back(output);
            }
        }

        if (wtx.IsCoinBase()) {
            if (wtx.GetDepthInMainChain() < 1) {
                entry.pushKV("category", "orphan");
            } else if (wtx.GetBlocksToMaturity() > 0) {
                entry.pushKV("category", "immature");
            } else {
                entry.pushKV("category", "coinbase");
            }
        } else if (!nFee) {
            entry.pushKV("category", "receive");
        } else if (amount == 0) {
            if (listSent.empty())
                entry.pushKV("fee", ValueFromAmount(-nFee));
            entry.pushKV("category", "internal_transfer");
        } else {
            entry.pushKV("category", "send");
        }
    };

    entry.pushKV("outputs", outputs);
    entry.pushKV("amount", ValueFromAmount(amount));

    if (search != "") {
        // search in addresses
        if (std::any_of(addresses.begin(), addresses.end(), [search](std::string addr) {
            return addr.find(search) != std::string::npos;
        })) {
            entries.push_back(entry);
            return ;
        }
        // search in amounts
        // character DOT '.' is not searched for: search "123" will find 1.23 and 12.3
        if (std::any_of(amounts.begin(), amounts.end(), [search](std::string amount) {
            return amount.find(search) != std::string::npos;
        })) {
            entries.push_back(entry);
            return ;
        }
    } else {
        entries.push_back(entry);
    }
}

static void push(UniValue & entry, std::string key, UniValue const & value)
{
    if (entry[key].getType() == 0) {
        entry.push_back(Pair(key, value));
    }
}

static void ParseRecords(
    UniValue &                 entries,
    const uint256 &            hash,
    const CTransactionRecord & rtx,
    CHDWallet * const          pwallet,
    const isminefilter &       watchonly_filter,
    std::string                search
) {
    std::vector<std::string> addresses;
    std::vector<std::string> amounts;
    UniValue   entry(UniValue::VOBJ);
    UniValue outputs(UniValue::VARR);
    size_t  nOwned      = 0;
    size_t  nFrom       = 0;
    size_t  nWatchOnly  = 0;
    CAmount totalAmount = 0;

    int confirmations = pwallet->GetDepthInMainChain(rtx.blockHash);
    push(entry, "confirmations", confirmations);
    if (confirmations > 0) {
        push(entry, "blockhash", rtx.blockHash.GetHex());
        push(entry, "blockindex", rtx.nIndex);
        push(entry, "blocktime", mapBlockIndex[rtx.blockHash]->GetBlockTime());
    } else {
        push(entry, "trusted", pwallet->IsTrusted(hash, rtx.blockHash));
    };

    push(entry, "txid", hash.ToString());
    UniValue conflicts(UniValue::VARR);
    std::set<uint256> setconflicts = pwallet->GetConflicts(hash);
    setconflicts.erase(hash);
    for (const auto &conflict : setconflicts) {
        conflicts.push_back(conflict.GetHex());
    }
    if (conflicts.size() > 0) {
        push(entry, "walletconflicts", conflicts);
    }
    PushTime(entry, "time", rtx.nTimeReceived);

    size_t nLockedOutputs = 0;
    for (auto &record : rtx.vout) {

        UniValue output(UniValue::VOBJ);

        if (record.nFlags & ORF_CHANGE) {
            continue ;
        }
        if (record.nFlags & ORF_OWN_ANY) {
            nOwned++;
        }
        if (record.nFlags & ORF_FROM) {
            nFrom++;
        }
        if (record.nFlags & ORF_OWN_WATCH) {
            nWatchOnly++;
        }
        if (record.nFlags & ORF_LOCKED) {
            nLockedOutputs++;
        }

        CBitcoinAddress addr;
        CTxDestination  dest;
        bool extracted = ExtractDestination(record.scriptPubKey, dest);

        // get account name
        if (extracted && !record.scriptPubKey.IsUnspendable()) {
            addr.Set(dest);
            std::map<CTxDestination, CAddressBookData>::iterator mai;
            mai = pwallet->mapAddressBook.find(dest);
            if (mai != pwallet->mapAddressBook.end() && !mai->second.name.empty()) {
                push(output, "account", mai->second.name);
            }
        };

        // stealth addresses
        CStealthAddress sx;
        if (record.vPath.size() > 0) {
            if (record.vPath[0] == ORA_STEALTH) {
                if (record.vPath.size() < 5) {
                    LogPrintf("%s: Warning, malformed vPath.", __func__);
                } else {
                    uint32_t sidx;
                    memcpy(&sidx, &record.vPath[1], 4);
                    if (pwallet->GetStealthByIndex(sidx, sx)) {
                        push(output, "stealth_address", sx.Encoded());
                        addresses.push_back(sx.Encoded());
                    }
                }
            }
        } else {
            if (extracted && dest.type() == typeid(CKeyID)) {
                CKeyID idK = boost::get<CKeyID>(dest);
                if (pwallet->GetStealthLinked(idK, sx)) {
                    push(output, "stealth_address", sx.Encoded());
                    addresses.push_back(sx.Encoded());
                }
            }
        }

        if (extracted && dest.type() == typeid(CNoDestination)) {
            push(output, "address", "none");
        } else if (extracted) {
            push(output, "address", addr.ToString());
            addresses.push_back(addr.ToString());
        }

        push(output, "type",
              record.nType == OUTPUT_STANDARD ? "standard"
            : "unknown");

        if (!record.sNarration.empty()) {
            push(output, "narration", record.sNarration);
        }

        CAmount amount = record.nValue;
        if (!(record.nFlags & ORF_OWN_ANY)) {
            amount *= -1;
        }
        totalAmount += amount;
        amounts.push_back(std::to_string(ValueFromAmount(amount).get_real()));
        push(output, "amount", ValueFromAmount(amount));
        push(output, "vout", record.n);
        outputs.push_back(output);
    }

    if (nFrom > 0) {
        push(entry, "abandoned", rtx.IsAbandoned());
        push(entry, "fee", ValueFromAmount(-rtx.nFee));
    }

    if (nOwned && nFrom) {
        push(entry, "category", "internal_transfer");
    } else if (nOwned) {
        push(entry, "category", "receive");
    } else if (nFrom) {
        push(entry, "category", "send");
    } else {
        push(entry, "category", "unknown");
    }

    if (nLockedOutputs) {
        push(entry, "requires_unlock", "true");
    }
    if (nWatchOnly) {
        push(entry, "involvesWatchonly", "true");
    }

    push(entry, "outputs", outputs);

    push(entry, "amount", ValueFromAmount(totalAmount));
    amounts.push_back(std::to_string(ValueFromAmount(totalAmount).get_real()));

    if (search != "") {
        // search in addresses
        if (std::any_of(addresses.begin(), addresses.end(), [search](std::string addr) {
            return addr.find(search) != std::string::npos;
        })) {
            entries.push_back(entry);
            return ;
        }
        // search in amounts
        // character DOT '.' is not searched for: search "123" will find 1.23 and 12.3
        if (std::any_of(amounts.begin(), amounts.end(), [search](std::string amount) {
            return amount.find(search) != std::string::npos;
        })) {
            entries.push_back(entry);
            return ;
        }
    } else {
        entries.push_back(entry);
    }
}

static std::string getAddress(UniValue const & transaction)
{
    if (transaction["stealth_address"].getType() != 0) {
        return transaction["stealth_address"].get_str();
    }
    if (transaction["address"].getType() != 0) {
        return transaction["address"].get_str();
    }
    if (transaction["outputs"][0]["stealth_address"].getType() != 0) {
        return transaction["outputs"][0]["stealth_address"].get_str();
    }
    if (transaction["outputs"][0]["address"].getType() != 0) {
        return transaction["outputs"][0]["address"].get_str();
    }
    return std::string();
}

UniValue filtertransactions(const JSONRPCRequest &request)
{
    CHDWallet * const pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "filtertransactions ( options )\n"
            "List transactions.\n"
            "1. options (json, optional) : A configuration object for the query\n"
            "\n"
            "        All keys are optional. Default values are:\n"
            "        {\n"
            "                \"count\":             10,\n"
            "                \"skip\":              0,\n"
            "                \"include_watchonly\": false,\n"
            "                \"search\":            ''\n"
            "                \"category\":          'all',\n"
            "                \"type\":              'all',\n"
            "                \"sort\":              'time'\n"
            "                \"from\":              '0'\n"
            "                \"to\":                '9999'\n"
            "                \"collate\":           false\n"
            "                \"with_reward\":       false\n"
            "                \"use_bech32\":        false\n"
            "        }\n"
            "\n"
            "        Expected values are as follows:\n"
            "                count:             number of transactions to be displayed\n"
            "                                   (integer >= 0, use 0 for unlimited)\n"
            "                skip:              number of transactions to skip\n"
            "                                   (integer >= 0)\n"
            "                include_watchonly: whether to include watchOnly transactions\n"
            "                                   (bool string)\n"
            "                search:            a query to search addresses and amounts\n"
            "                                   character DOT '.' is not searched for:\n"
            "                                   search \"123\" will find 1.23 and 12.3\n"
            "                                   (query string)\n"
            "                category:          select only one category of transactions to return\n"
            "                                   (string from list)\n"
            "                                   all, send, orphan, immature, coinbase, receive,\n"
            "                                   internal_transfer\n"
            "                type:              select only one type of transactions to return\n"
            "                                   (string from list)\n"
            "                                   all, standard, anon, blind\n"
            "                sort:              sort transactions by criteria\n"
            "                                   (string from list)\n"
            "                                   time          most recent first\n"
            "                                   address       alphabetical\n"
            "                                   category      alphabetical\n"
            "                                   amount        biggest first\n"
            "                                   confirmations most confirmations first\n"
            "                                   txid          alphabetical\n"
            "                from:              unix timestamp or string \"yyyy-mm-ddThh:mm:ss\"\n"
            "                to:                unix timestamp or string \"yyyy-mm-ddThh:mm:ss\"\n"
            "                collate:           display number of records and sum of amount fields\n"
            "                with_reward        calculate reward explicitly from txindex if necessary\n"
            "                use_bech32         display addresses in bech32 encoding\n"
            "\n"
            "        Examples:\n"
            "            Multiple arguments\n"
            "                " + HelpExampleCli("filtertransactions", "\"{\\\"sort\\\":\\\"amount\\\", \\\"category\\\":\\\"receive\\\"}\"") + "\n"
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    unsigned int count     = 10;
    int          skip      = 0;
    isminefilter watchonly = ISMINE_SPENDABLE;
    std::string  search    = "";
    std::string  category  = "all";
    std::string  type      = "all";
    std::string  sort      = "time";

    int64_t timeFrom = 0;
    int64_t timeTo = 0x3AFE130E00; // 9999
    bool fCollate = false;
    bool fWithReward = false;
    bool fBech32 = false;

    if (!request.params[0].isNull()) {
        const UniValue & options = request.params[0].get_obj();
        RPCTypeCheckObj(options,
            {
                {"count",             UniValueType(UniValue::VNUM)},
                {"skip",              UniValueType(UniValue::VNUM)},
                {"include_watchonly", UniValueType(UniValue::VBOOL)},
                {"search",            UniValueType(UniValue::VSTR)},
                {"category",          UniValueType(UniValue::VSTR)},
                {"type",              UniValueType(UniValue::VSTR)},
                {"sort",              UniValueType(UniValue::VSTR)},
                {"collate",           UniValueType(UniValue::VBOOL)},
                {"with_reward",       UniValueType(UniValue::VBOOL)},
                {"use_bech32",        UniValueType(UniValue::VBOOL)},
            },
            true, // allow null
            false // strict
        );
        if (options.exists("count")) {
            int _count = options["count"].get_int();
            if (_count < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid count: %i.", _count));
            }
            count = _count;
        }
        if (options.exists("skip")) {
            skip = options["skip"].get_int();
            if (skip < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid skip number: %i.", skip));
            }
        }
        if (options.exists("include_watchonly")) {
            if (options["include_watchonly"].get_bool()) {
                watchonly = watchonly | ISMINE_WATCH_ONLY;
            }
        }
        if (options.exists("search")) {
            search = options["search"].get_str();
        }
        if (options.exists("category")) {
            category = options["category"].get_str();
            std::vector<std::string> categories = {
                "all",
                "send",
                "orphan",
                "immature",
                "coinbase",
                "receive",
                "internal_transfer"
            };
            auto it = std::find(categories.begin(), categories.end(), category);
            if (it == categories.end()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid category: %s.", category));
            }
        }
        if (options.exists("type")) {
            type = options["type"].get_str();
            std::vector<std::string> types = {
                "all",
                "standard",
                "zerocoin"
            };
            auto it = std::find(types.begin(), types.end(), type);
            if (it == types.end()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid type: %s.", type));
            }
        }
        if (options.exists("sort")) {
            sort = options["sort"].get_str();
            std::vector<std::string> sorts = {
                "time",
                "address",
                "category",
                "amount",
                "confirmations",
                "txid"
            };
            auto it = std::find(sorts.begin(), sorts.end(), sort);
            if (it == sorts.end()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("Invalid sort: %s.", sort));
            }
        }

        if (options["from"].isStr())
            timeFrom = nix::strToEpoch(options["from"].get_str().c_str());
        else
        if (options["from"].isNum())
            timeFrom = options["from"].get_int64();
        if (options["to"].isStr())
            timeTo = nix::strToEpoch(options["to"].get_str().c_str(), true);
        else
        if (options["to"].isNum())
            timeTo = options["to"].get_int64();
        if (options["collate"].isBool())
            fCollate = options["collate"].get_bool();
        if (options["with_reward"].isBool())
            fWithReward = options["with_reward"].get_bool();
        if (options["use_bech32"].isBool())
            fBech32 = options["use_bech32"].get_bool();
    }

    // for transactions and records
    UniValue transactions(UniValue::VARR);

    // transaction processing
    const CHDWallet::TxItems &txOrdered = pwallet->wtxOrdered;
    CWallet::TxItems::const_reverse_iterator tit = txOrdered.rbegin();
    while (tit != txOrdered.rend()) {
        CWalletTx* const pwtx = tit->second.first;
        int64_t txTime = pwtx->GetTxTime();
        if (txTime < timeFrom) break;
        if (txTime <= timeTo)
            ParseOutputs(
                transactions,
                *pwtx,
                pwallet,
                watchonly,
                search,
                fWithReward,
                fBech32
            );
        tit++;
    }

    // records processing
    const RtxOrdered_t &rtxOrdered = pwallet->rtxOrdered;
    RtxOrdered_t::const_reverse_iterator rit = rtxOrdered.rbegin();
    while (rit != rtxOrdered.rend()) {
        const uint256 &hash = rit->second->first;
        const CTransactionRecord &rtx = rit->second->second;
        int64_t txTime = rtx.GetTxTime();
        if (txTime < timeFrom) break;
        if (txTime <= timeTo)
            ParseRecords(
                transactions,
                hash,
                rtx,
                pwallet,
                watchonly,
                search
            );
        rit++;
    }

    // sort
    std::vector<UniValue> values = transactions.getValues();
    std::sort(values.begin(), values.end(), [sort] (UniValue a, UniValue b) -> bool {
        std::string a_address = getAddress(a);
        std::string b_address = getAddress(b);
        double a_amount =   a["category"].get_str() == "send"
                        ? -(a["amount"  ].get_real())
                        :   a["amount"  ].get_real();
        double b_amount =   b["category"].get_str() == "send"
                        ? -(b["amount"  ].get_real())
                        :   b["amount"  ].get_real();
        return (
              sort == "address"
                ? a_address < b_address
            : sort == "category" || sort == "txid"
                ? a[sort].get_str() < b[sort].get_str()
            : sort == "time" || sort == "confirmations"
                ? a[sort].get_real() > b[sort].get_real()
            : sort == "amount"
                ? a_amount > b_amount
            : false
        );
    });

    // filter, skip, count and sum
    CAmount nTotalAmount = 0, nTotalReward = 0;
    UniValue result(UniValue::VARR);
    if (count == 0) {
        count = values.size();
    }
    // for every value while count is positive
    for (unsigned int i = 0; i < values.size() && count != 0; i++) {
        // if value's category is relevant
        if (values[i]["category"].get_str() == category || category == "all") {
            // if value's type is not relevant
            if (values[i]["type"].getType() == 0) {
                // value's type is undefined
                if (!(type == "all" || type == "standard")) {
                    // type is not 'all' or 'standard'
                    continue ;
                }
            } else if (!(values[i]["type"].get_str() == type || type == "all")) {
                // value's type is defined
                // value's type is not type or 'all'
                continue ;
            }
            // if we've skipped enough valid values
            if (skip-- <= 0) {
                result.push_back(values[i]);
                count--;

                if (fCollate) {
                    if (!values[i]["amount"].isNull())
                        nTotalAmount += AmountFromValue(values[i]["amount"]);
                    if (!values[i]["reward"].isNull())
                        nTotalReward += AmountFromValue(values[i]["reward"]);
                };
            }
        }
    }

    if (fCollate) {
        UniValue retObj(UniValue::VOBJ);
        UniValue stats(UniValue::VOBJ);
        stats.pushKV("records", (int)result.size());
        stats.pushKV("total_amount", ValueFromAmount(nTotalAmount));
        if (fWithReward)
            stats.pushKV("total_reward", ValueFromAmount(nTotalReward));
        retObj.pushKV("tx", result);
        retObj.pushKV("collated", stats);
        return retObj;
    };

    return result;
}

enum SortCodes
{
    SRT_LABEL_ASC,
    SRT_LABEL_DESC,
};

class AddressComp {
public:
    int nSortCode;
    AddressComp(int nSortCode_) : nSortCode(nSortCode_) {}
    bool operator() (
        const std::map<CTxDestination, CAddressBookData>::iterator a,
        const std::map<CTxDestination, CAddressBookData>::iterator b) const
    {
        switch (nSortCode)
        {
            case SRT_LABEL_DESC:
                return b->second.name.compare(a->second.name) < 0;
            default:
                break;
        };
        //default: case SRT_LABEL_ASC:
        return a->second.name.compare(b->second.name) < 0;
    }
};

UniValue filteraddresses(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 6)
        throw std::runtime_error(
            "filteraddresses ( offset count sort_code \"match_str\" match_owned show_path )\n"
            "List addresses."
            "filteraddresses offset count will list 'count' addresses starting from 'offset'\n"
            "filteraddresses -1 will count addresses\n"
            "sort_code 0 sort by label ascending, 1 sort by label descending, default 0\n"
            "\"match_str]\" filter by label\n"
            "match_owned 0 off, 1 owned, 2 non-owned, default 0\n"
            );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    int nOffset = 0, nCount = 0x7FFFFFFF;
    if (request.params.size() > 0)
        nOffset = request.params[0].get_int();

    std::map<CTxDestination, CAddressBookData>::iterator it;
    if (request.params.size() == 1 && nOffset == -1)
    {
        LOCK(pwallet->cs_wallet);
        // Count addresses
        UniValue result(UniValue::VOBJ);

        result.pushKV("total", (int)pwallet->mapAddressBook.size());

        int nReceive = 0, nSend = 0;
        for (it = pwallet->mapAddressBook.begin(); it != pwallet->mapAddressBook.end(); ++it)
        {
            if (it->second.nOwned == 0)
                it->second.nOwned = pwallet->HaveAddress(it->first) ? 1 : 2;

            if (it->second.nOwned == 1)
                nReceive++;
            else
            if (it->second.nOwned == 2)
                nSend++;
        };

        result.pushKV("num_receive", nReceive);
        result.pushKV("num_send", nSend);
        return result;
    };

    if (request.params.size() > 1)
        nCount = request.params[1].get_int();

    if (nOffset < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "offset must be 0 or greater.");
    if (nCount < 1)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be 1 or greater.");


    // TODO: Make better
    int nSortCode = SRT_LABEL_ASC;
    if (request.params.size() > 2)
    {
        std::string sCode = request.params[2].get_str();
        if (sCode == "0")
            nSortCode = SRT_LABEL_ASC;
        else
        if (sCode == "1")
            nSortCode = SRT_LABEL_DESC;
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown sort_code.");
    };

    int nMatchOwned = 0; // 0 off/all, 1 owned, 2 non-owned
    int nMatchMode = 0; // 1 contains
    int nShowPath = 1;

    std::string sMatch;
    if (request.params.size() > 3)
        sMatch = request.params[3].get_str();

    if (sMatch != "")
        nMatchMode = 1;

    if (request.params.size() > 4)
    {
        std::string s = request.params[4].get_str();
        if (s != "")
            nMatchOwned = std::stoi(s);
    };

    if (request.params.size() > 5)
    {
        std::string s = request.params[5].get_str();
        bool fTemp;
        if (!nix::GetStringBool(s, fTemp))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown argument for show_path: %s.", s.c_str()));
        nShowPath = !fTemp ? 0 : nShowPath;
    };

    UniValue result(UniValue::VARR);
    {
        LOCK(pwallet->cs_wallet);

        CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");

        if (nOffset >= (int)pwallet->mapAddressBook.size())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("offset is beyond last address (%d).", nOffset));
        std::vector<std::map<CTxDestination, CAddressBookData>::iterator> vitMapAddressBook;
        vitMapAddressBook.reserve(pwallet->mapAddressBook.size());

        for (it = pwallet->mapAddressBook.begin(); it != pwallet->mapAddressBook.end(); ++it)
        {
            if (it->second.nOwned == 0)
                it->second.nOwned = pwallet->HaveAddress(it->first) ? 1 : 2;

            if (nMatchOwned && it->second.nOwned != nMatchOwned)
                continue;

            if (nMatchMode)
            {
                if (!nix::stringsMatchI(it->second.name, sMatch, nMatchMode-1))
                    continue;
            };

            vitMapAddressBook.push_back(it);
        };

        std::sort(vitMapAddressBook.begin(), vitMapAddressBook.end(), AddressComp(nSortCode));

        std::map<uint32_t, std::string> mapKeyIndexCache;
        std::vector<std::map<CTxDestination, CAddressBookData>::iterator>::iterator vit;
        int nEntries = 0;
        for (vit = vitMapAddressBook.begin()+nOffset;
            vit != vitMapAddressBook.end() && nEntries < nCount; ++vit)
        {
            auto &item = *vit;
            UniValue entry(UniValue::VOBJ);

            CBitcoinAddress address(item->first, item->second.fBech32);
            entry.pushKV("address", address.ToString());
            entry.pushKV("label", item->second.name);
            entry.pushKV("owned", item->second.nOwned == 1 ? "true" : "false");

            if (nShowPath > 0)
            {
                if (item->second.vPath.size() > 0)
                {
                    uint32_t index = item->second.vPath[0];
                    std::map<uint32_t, std::string>::iterator mi = mapKeyIndexCache.find(index);

                    if (mi != mapKeyIndexCache.end())
                    {
                        entry.pushKV("root", mi->second);
                    } else
                    {
                        CKeyID accId;
                        if (!wdb.ReadExtKeyIndex(index, accId))
                        {
                            entry.pushKV("root", "error");
                        } else
                        {
                            CBitcoinAddress addr;
                            addr.Set(accId, CChainParams::EXT_ACC_HASH);
                            std::string sTmp = addr.ToString();
                            entry.pushKV("root", sTmp);
                            mapKeyIndexCache[index] = sTmp;
                        };
                    };
                };

                if (item->second.vPath.size() > 1)
                {
                    std::string sPath;
                    if (0 == PathToString(item->second.vPath, sPath, '\'', 1))
                        entry.pushKV("path", sPath);
                };
            };

            result.push_back(entry);
            nEntries++;
        };
    } // cs_wallet

    return result;
}

UniValue manageaddressbook(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4)
        throw std::runtime_error(
            "manageaddressbook \"action\" \"address\" ( \"label\" \"purpose\" )\n"
            "Manage the address book."
            "\nArguments:\n"
            "1. \"action\"      (string, required) 'add/edit/del/info/newsend' The action to take.\n"
            "2. \"address\"     (string, required) The address to affect.\n"
            "3. \"label\"       (string, optional) Optional label.\n"
            "4. \"purpose\"     (string, optional) Optional purpose label.\n");

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    std::string sAction = request.params[0].get_str();
    std::string sAddress = request.params[1].get_str();
    std::string sLabel, sPurpose;

    if (sAction != "info")
        EnsureWalletIsUnlocked(pwallet);

    bool fHavePurpose = false;
    if (request.params.size() > 2)
        sLabel = request.params[2].get_str();
    if (request.params.size() > 3)
    {
        sPurpose = request.params[3].get_str();
        fHavePurpose = true;
    };

    CBitcoinAddress address(sAddress);

    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Invalid NIX address."));

    CTxDestination dest = address.Get();

    std::map<CTxDestination, CAddressBookData>::iterator mabi;
    mabi = pwallet->mapAddressBook.find(dest);

    std::vector<uint32_t> vPath;

    UniValue objDestData(UniValue::VOBJ);

    if (sAction == "add")
    {
        if (mabi != pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is recorded in the address book."), sAddress));

        if (!pwallet->SetAddressBook(nullptr, dest, sLabel, sPurpose, vPath, true))
            throw JSONRPCError(RPC_WALLET_ERROR, "SetAddressBook failed.");
    } else
    if (sAction == "edit")
    {
        if (request.params.size() < 3)
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Need a parameter to change."));
        if (mabi == pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is not in the address book."), sAddress));

        if (!pwallet->SetAddressBook(nullptr, dest, sLabel,
            fHavePurpose ? sPurpose : mabi->second.purpose, mabi->second.vPath, true))
            throw JSONRPCError(RPC_WALLET_ERROR, "SetAddressBook failed.");

        sLabel = mabi->second.name;
        sPurpose = mabi->second.purpose;

        for (const auto &pair : mabi->second.destdata)
            objDestData.pushKV(pair.first, pair.second);

    } else
    if (sAction == "del")
    {
        if (mabi == pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is not in the address book."), sAddress));
        sLabel = mabi->second.name;
        sPurpose = mabi->second.purpose;

        if (!pwallet->DelAddressBook(dest))
            throw JSONRPCError(RPC_WALLET_ERROR, "DelAddressBook failed.");
    } else
    if (sAction == "info")
    {
        if (mabi == pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is not in the address book."), sAddress));

        UniValue result(UniValue::VOBJ);

        result.pushKV("action", sAction);
        result.pushKV("address", sAddress);

        result.pushKV("label", mabi->second.name);
        result.pushKV("purpose", mabi->second.purpose);

        if (mabi->second.nOwned == 0)
            mabi->second.nOwned = pwallet->HaveAddress(mabi->first) ? 1 : 2;

        result.pushKV("owned", mabi->second.nOwned == 1 ? "true" : "false");

        if (mabi->second.vPath.size() > 1)
        {
            std::string sPath;
            if (0 == PathToString(mabi->second.vPath, sPath, '\'', 1))
                result.pushKV("path", sPath);
        };

        for (const auto &pair : mabi->second.destdata)
            objDestData.pushKV(pair.first, pair.second);
        if (objDestData.size() > 0)
            result.pushKV("destdata", objDestData);

        result.pushKV("result", "success");

        return result;
    } else
    if (sAction == "newsend")
    {
        // Only update the purpose field if address does not yet exist
        if (mabi != pwallet->mapAddressBook.end())
            sPurpose = "";// "" means don't change purpose

        if (!pwallet->SetAddressBook(dest, sLabel, sPurpose))
            throw JSONRPCError(RPC_WALLET_ERROR, "SetAddressBook failed.");

        if (mabi != pwallet->mapAddressBook.end())
            sPurpose = mabi->second.purpose;
    } else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Unknown action, must be one of 'add/edit/del'."));
    };

    UniValue result(UniValue::VOBJ);

    result.pushKV("action", sAction);
    result.pushKV("address", sAddress);

    if (sLabel.size() > 0)
        result.pushKV("label", sLabel);
    if (sPurpose.size() > 0)
        result.pushKV("purpose", sPurpose);
    if (objDestData.size() > 0)
        result.pushKV("destdata", objDestData);

    result.pushKV("result", "success");

    return result;
}

extern double GetDifficulty(const CBlockIndex* blockindex = nullptr);

static int AddOutput(uint8_t nType, std::vector<CTempRecipient> &vecSend, const CTxDestination &address, CAmount nValue,
    bool fSubtractFeeFromAmount, std::string &sNarr, std::string &sError)
{
    CTempRecipient r;
    r.nType = nType;
    r.SetAmount(nValue);
    r.fSubtractFeeFromAmount = fSubtractFeeFromAmount;
    r.address = address;
    r.sNarration = sNarr;

    vecSend.push_back(r);
    return 0;
};

static UniValue SendToInner(const JSONRPCRequest &request, OutputTypes typeIn, OutputTypes typeOut)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    if (!request.fSkipBlock)
        pwallet->BlockUntilSyncedToCurrentChain();

    EnsureWalletIsUnlocked(pwallet);

    if (pwallet->GetBroadcastTransactions() && !g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");


    CAmount nTotal = 0;

    std::vector<CTempRecipient> vecSend;
    std::string sError;

    size_t nCommentOfs = 2;
    size_t nTestFeeOfs = 99;
    size_t nCoinControlOfs = 99;

    if (request.params[0].isArray())
    {
        const UniValue &outputs = request.params[0].get_array();

        for (size_t k = 0; k < outputs.size(); ++k)
        {
            if (!outputs[k].isObject())
                throw JSONRPCError(RPC_TYPE_ERROR, "Not an object");
            const UniValue &obj = outputs[k].get_obj();

            std::string sAddress;
            CAmount nAmount;

            if (obj.exists("address"))
                sAddress = obj["address"].get_str();
            else
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Must provide an address.");

            CBitcoinAddress address(sAddress);

            if (!address.IsValidStealthAddress())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid NIX stealth address");

            if (!obj.exists("script") && !address.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid NIX address");

            if (obj.exists("amount"))
                nAmount = AmountFromValue(obj["amount"]);
            else
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Must provide an amount.");

            if (nAmount <= 0)
                throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
            nTotal += nAmount;

            bool fSubtractFeeFromAmount = false;
            if (obj.exists("subfee"))
                fSubtractFeeFromAmount = obj["subfee"].get_bool();

            std::string sNarr;
            if (obj.exists("narr"))
                sNarr = obj["narr"].get_str();

            if (0 != AddOutput(typeOut, vecSend, address.Get(), nAmount, fSubtractFeeFromAmount, sNarr, sError))
                throw JSONRPCError(RPC_MISC_ERROR, strprintf("AddOutput failed: %s.", sError));

            if (obj.exists("script"))
            {
                CTempRecipient &r = vecSend.back();

                if (sAddress != "script")
                    JSONRPCError(RPC_INVALID_PARAMETER, "address parameter must be 'script' to set script explicitly.");

                std::string sScript = obj["script"].get_str();
                std::vector<uint8_t> scriptData = ParseHex(sScript);
                r.scriptPubKey = CScript(scriptData.begin(), scriptData.end());
                r.fScriptSet = true;

                if (typeOut != OUTPUT_STANDARD)
                    throw std::runtime_error("In progress, setting script only works for standard outputs.");
            };
        };
        nCommentOfs = 1;
        nTestFeeOfs = 5;
        nCoinControlOfs = 6;
    } else
    {
        std::string sAddress = request.params[0].get_str();
        CBitcoinAddress address(sAddress);

        if (!address.IsValidStealthAddress())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid NIX stealth address");

        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid NIX address");

        CAmount nAmount = AmountFromValue(request.params[1]);
        if (nAmount <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
        nTotal += nAmount;

        bool fSubtractFeeFromAmount = false;
        if (request.params.size() > 4)
            fSubtractFeeFromAmount = request.params[4].get_bool();

        std::string sNarr;
        if (request.params.size() > 5)
        {
            sNarr = request.params[5].get_str();
            if (sNarr.length() > 24)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Narration can range from 1 to 24 characters.");
        };

        if (0 != AddOutput(typeOut, vecSend, address.Get(), nAmount, fSubtractFeeFromAmount, sNarr, sError))
            throw JSONRPCError(RPC_MISC_ERROR, strprintf("AddOutput failed: %s.", sError));
    };

    switch (typeIn)
    {
        case OUTPUT_STANDARD:
            if (nTotal > pwallet->GetBalance())
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");
            break;
        default:
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown input type: %d.", typeIn));
    };

    // Wallet comments
    CWalletTx wtx;
    CTransactionRecord rtx;

    size_t nv = nCommentOfs;
    if (request.params.size() > nv && !request.params[nv].isNull())
    {
        std::string s = request.params[nv].get_str();
        nix::TrimQuotes(s);
        if (!s.empty())
        {
            std::vector<uint8_t> v(s.begin(), s.end());
            wtx.mapValue["comment"] = s;
            rtx.mapValue[RTXVT_COMMENT] = v;
        };
    };
    nv++;
    if (request.params.size() > nv && !request.params[nv].isNull())
    {
        std::string s = request.params[nv].get_str();
        nix::TrimQuotes(s);
        if (!s.empty())
        {
            std::vector<uint8_t> v(s.begin(), s.end());
            wtx.mapValue["to"] = s;
            rtx.mapValue[RTXVT_TO] = v;
        };
    };

    nv++;
    size_t nInputsPerSig = 64;
    if (request.params.size() > nv)
        nInputsPerSig = request.params[nv].get_int();

    bool fShowHex = false;
    bool fCheckFeeOnly = false;
    nv = nTestFeeOfs;
    if (request.params.size() > nv)
        fCheckFeeOnly = request.params[nv].get_bool();


    CCoinControl coincontrol;

    nv = nCoinControlOfs;
    if (request.params.size() > nv
        && request.params[nv].isObject())
    {
        const UniValue &uvCoinControl = request.params[nv].get_obj();

        if (uvCoinControl.exists("changeaddress"))
        {
            std::string sChangeAddress = uvCoinControl["changeaddress"].get_str();

            // Check for script
            bool fHaveScript = false;
            if (IsHex(sChangeAddress))
            {
                std::vector<uint8_t> vScript = ParseHex(sChangeAddress);
                CScript script(vScript.begin(), vScript.end());

                txnouttype whichType;
                if (IsStandard(script, whichType, true))
                {
                    coincontrol.scriptChange = script;
                    fHaveScript = true;
                };
            };

            if (!fHaveScript)
            {
                CBitcoinAddress addrChange(sChangeAddress);
                coincontrol.destChange = addrChange.Get();
            };
        };

        const UniValue &uvInputs = uvCoinControl["inputs"];
        if (uvInputs.isArray())
        {
            for (size_t i = 0; i < uvInputs.size(); ++i)
            {
                const UniValue &uvi = uvInputs[i];
                RPCTypeCheckObj(uvi,
                {
                    {"tx", UniValueType(UniValue::VSTR)},
                    {"n", UniValueType(UniValue::VNUM)},
                });

                COutPoint op(uint256S(uvi["tx"].get_str()), uvi["n"].get_int());
                coincontrol.setSelected.insert(op);
            };
        };

        if (uvCoinControl.exists("feeRate") && uvCoinControl.exists("estimate_mode"))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and feeRate");
        if (uvCoinControl.exists("feeRate") && uvCoinControl.exists("conf_target"))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and feeRate");

        if (uvCoinControl.exists("replaceable"))
        {
            if (!uvCoinControl["replaceable"].isBool())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Replaceable parameter must be boolean.");
            coincontrol.signalRbf = uvCoinControl["replaceable"].get_bool();
        };

        if (uvCoinControl.exists("conf_target"))
        {
            if (!uvCoinControl["conf_target"].isNum())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "conf_target parameter must be numeric.");
            coincontrol.m_confirm_target = ParseConfirmTarget(uvCoinControl["conf_target"]);
        };

        if (uvCoinControl.exists("estimate_mode"))
        {
            if (!uvCoinControl["estimate_mode"].isStr())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "estimate_mode parameter must be a string.");
            if (!FeeModeFromString(uvCoinControl["estimate_mode"].get_str(), coincontrol.m_fee_mode))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        };

        if (uvCoinControl.exists("feeRate"))
        {
            coincontrol.m_feerate = CFeeRate(AmountFromValue(uvCoinControl["feeRate"]));
            coincontrol.fOverrideFeeRate = true;
        };

        if (uvCoinControl["debug"].isBool() && uvCoinControl["debug"].get_bool() == true)
            fShowHex = true;
    };


    CAmount nFeeRet = 0;
    switch (typeIn)
    {
        case OUTPUT_STANDARD:
            if (0 != pwallet->AddStandardInputs(wtx, rtx, vecSend, !fCheckFeeOnly, nFeeRet, &coincontrol, sError))
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("AddStandardInputs failed: %s.", sError));
            break;
        default:
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Unknown input type: %d.", typeIn));
    };

    if (fCheckFeeOnly)
    {
        UniValue result(UniValue::VOBJ);
        result.pushKV("fee", ValueFromAmount(nFeeRet));
        result.pushKV("bytes", (int)GetVirtualTransactionSize(*(wtx.tx)));
        result.pushKV("need_hwdevice", UniValue(coincontrol.fNeedHardwareKey ? true : false));

        if (fShowHex)
        {
            std::string strHex = EncodeHexTx(*(wtx.tx), RPCSerializationFlags());
            result.pushKV("hex", strHex);
        };

        UniValue objChangedOutputs(UniValue::VOBJ);
        std::map<std::string, CAmount> mapChanged; // Blinded outputs are split, join the values for display
        for (const auto &r : vecSend)
        {
            if (!r.fChange
                && r.nAmount != r.nAmountSelected)
            {
                std::string sAddr = CBitcoinAddress(r.address).ToString();

                if (mapChanged.count(sAddr))
                    mapChanged[sAddr] += r.nAmount;
                else
                    mapChanged[sAddr] = r.nAmount;
            };
        };

        for (const auto &v : mapChanged)
            objChangedOutputs.pushKV(v.first, v.second);

        result.pushKV("outputs_fee", objChangedOutputs);
        return result;
    };

    // Store sent narrations
    for (const auto &r : vecSend)
    {
        if (r.nType != OUTPUT_STANDARD
            || r.sNarration.size() < 1)
            continue;
        std::string sKey = strprintf("n%d", r.n);
        wtx.mapValue[sKey] = r.sNarration;
    };

    CValidationState state;
    CReserveKey reservekey(pwallet);
    if (typeIn == OUTPUT_STANDARD && typeOut == OUTPUT_STANDARD)
    {
        if (!pwallet->CommitTransaction(wtx, reservekey, g_connman.get(), state))
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Transaction commit failed: %s", FormatStateMessage(state)));
    } else
    {
        if (!pwallet->CommitTransaction(wtx, rtx, reservekey, g_connman.get(), state))
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Transaction commit failed: %s", FormatStateMessage(state)));
    };


    pwallet->PostProcessTempRecipients(vecSend);

    return wtx.GetHash().GetHex();
}

static const char *TypeToWord(OutputTypes type)
{
    switch (type)
    {
        case OUTPUT_STANDARD:
            return "nix";
        default:
            break;
    };
    return "unknown";
};

static OutputTypes WordToType(std::string &s)
{
    if (s == "nix")
        return OUTPUT_STANDARD;
    return OUTPUT_NULL;
};

static std::string SendHelp(CHDWallet *pwallet, OutputTypes typeIn, OutputTypes typeOut)
{
    std::string rv;

    std::string cmd = std::string("send") + TypeToWord(typeIn) + "to" + TypeToWord(typeOut);

    rv = cmd + " \"address\" amount ( \"comment\" \"comment-to\" subtractfeefromamount \"narration\"";
    rv += ")\n";

    rv += "\nSend an amount of ";
    rv += std::string(" nix") + ".\n";

    rv += HelpRequiringPassphrase(pwallet);

    rv +=   "\nArguments:\n"
            "1. \"address\"     (string, required) The NIX address to send to.\n"
            "2. \"amount\"      (numeric or string, required) The amount in " + CURRENCY_UNIT + " to send. eg 0.1\n"
            "3. \"comment\"     (string, optional) A comment used to store what the transaction is for. \n"
            "                            This is not part of the transaction, just kept in your wallet.\n"
            "4. \"comment_to\"  (string, optional) A comment to store the name of the person or organization \n"
            "                            to which you're sending the transaction. This is not part of the \n"
            "                            transaction, just kept in your wallet.\n"
            "5. subtractfeefromamount  (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
            "                            The recipient will receive less " + CURRENCY_UNIT + " than you enter in the amount field.\n"
            "6. \"narration\"   (string, optional) Up to 24 characters sent with the transaction.\n"
            "                            The narration is stored in the blockchain and is sent encrypted when destination is a stealth address and uncrypted otherwise.\n";
    rv +=
            "\nResult:\n"
            "\"txid\"           (string) The transaction id.\n";

    rv +=   "\nExamples:\n"
            + HelpExampleCli(cmd, "\"GPGyji8uZFip6H15GUfj6bsutRVLsCyBFL3P7k7T7MUDRaYU8GfwUHpfxonLFAvAwr2RkigyGfTgWMfzLAAP8KMRHq7RE8cwpEEekH\" 0.1");

    return rv;
};

UniValue sendtypeto(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;
    if (request.fHelp || request.params.size() < 3 || request.params.size() > 7)
        throw std::runtime_error(
            "sendtypeto \"typein\" \"typeout\" [{address: , amount: , narr: , subfee:},...] (\"comment\" \"comment-to\" inputs_per_sig test_fee coin_control)\n"
            "\nSend NIX to multiple outputs.\n"
            + HelpRequiringPassphrase(pwallet) +
            "\nArguments:\n"
            "1. \"typein\"          (string, required) nix\n"
            "2. \"typeout\"         (string, required) nix\n"
            "3. \"outputs\"         (json, required) Array of output objects\n"
            "    3.1 \"address\"    (string, required) The NIX address to send to.\n"
            "    3.2 \"amount\"     (numeric or string, required) The amount in " + CURRENCY_UNIT + " to send. eg 0.1\n"
            "    3.x \"narr\"       (string, optional) Up to 24 character narration sent with the transaction.\n"
            "    3.x \"subfee\"     (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
            "    3.x \"script\"     (string, optional) Hex encoded script, will override the address.\n"
            "4. \"comment\"         (string, optional) A comment used to store what the transaction is for. \n"
            "                            This is not part of the transaction, just kept in your wallet.\n"
            "5. \"comment_to\"      (string, optional) A comment to store the name of the person or organization \n"
            "                            to which you're sending the transaction. This is not part of the \n"
            "                            transaction, just kept in your wallet.\n"
            "6. test_fee         (bool, optional, default=false) Only return the fee it would cost to send, txn is discarded.\n"
            "7. coin_control     (json, optional) Coincontrol object.\n"
            "   {\"changeaddress\": ,\n"
            "    \"inputs\": [{\"tx\":, \"n\":},...],\n"
            "    \"replaceable\": boolean,\n"
            "       Allow this transaction to be replaced by a transaction with higher fees via BIP 125\n"
            "    \"conf_target\": numeric,\n"
            "       Confirmation target (in blocks)\n"
            "    \"estimate_mode\": string,\n"
            "       The fee estimate mode, must be one of:\n"
            "           \"UNSET\"\n"
            "           \"ECONOMICAL\"\n"
            "           \"CONSERVATIVE\"\n"
            "     \"feeRate\"                (numeric, optional, default not set: makes wallet determine the fee) Set a specific feerate (" + CURRENCY_UNIT + " per KB)\n"
            "   }\n"
            "\nResult:\n"
            "\"txid\"              (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("sendtypeto", "nix \"[{\\\"address\\\":\\\"NipVcjgYatnkKgveaeqhkeQBFwjqR7jKBR\\\",\\\"amount\\\":0.1}]\""));

    std::string sTypeIn = request.params[0].get_str();
    std::string sTypeOut = request.params[1].get_str();

    OutputTypes typeIn = WordToType(sTypeIn);
    OutputTypes typeOut = WordToType(sTypeOut);

    if (typeIn == OUTPUT_NULL)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown input type.");
    if (typeOut == OUTPUT_NULL)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown output type.");

    JSONRPCRequest req = request;
    req.params.erase(0, 2);

    return SendToInner(req, typeIn, typeOut);
};

UniValue buildscript(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "buildscript json\n"
            "\nArguments:\n"
            "{recipe: , ...}\n"
            "\nRecipes:\n"
            "{\"recipe\":\"abslocktime\", \"time\":timestamp, \"addr\":\"addr\"}"
            "{\"recipe\":\"rellocktime\", \"time\":timestamp, \"addr\":\"addr\"}"
            );

    if (!request.params[0].isObject())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Input must be a json object.");

    const UniValue &params = request.params[0].get_obj();

    const UniValue &recipe = params["recipe"];
    if (!recipe.isStr())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing recipe.");

    std::string sRecipe = recipe.get_str();

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("recipe", sRecipe);

    CScript scriptOut;

    if (sRecipe == "abslocktime")
    {
        RPCTypeCheckObj(params,
        {
            {"time", UniValueType(UniValue::VNUM)},
            {"addr", UniValueType(UniValue::VSTR)},
        });

        CBitcoinAddress addr(params["addr"].get_str());
        if (!addr.IsValid())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid addr.");

        CScript scriptAddr = GetScriptForDestination(addr.Get());

        scriptOut = CScript() << params["time"].get_int64() << OP_CHECKLOCKTIMEVERIFY << OP_DROP;
        scriptOut += scriptAddr;
    } else
    if (sRecipe == "rellocktime")
    {
        RPCTypeCheckObj(params,
        {
            {"time", UniValueType(UniValue::VNUM)},
            {"addr", UniValueType(UniValue::VSTR)},
        });

        CBitcoinAddress addr(params["addr"].get_str());
        if (!addr.IsValid())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid addr.");

        CScript scriptAddr = GetScriptForDestination(addr.Get());

        scriptOut = CScript() << params["time"].get_int64() << OP_CHECKSEQUENCEVERIFY << OP_DROP;
        scriptOut += scriptAddr;
    } else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown recipe.");
    };

    obj.pushKV("hex", HexStr(scriptOut.begin(), scriptOut.end()));
    obj.pushKV("asm", ScriptToAsmStr(scriptOut));

    return obj;
};

UniValue createsignaturewithwallet(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 4)
        throw std::runtime_error(
            "createsignaturewithwallet \"hexstring\" \"prevtx\" \"address\" \"sighashtype\"\n"
            "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
            + HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"hexstring\"                      (string, required) The transaction hex string\n"
            "2. \"prevtx\"                         (json, required) The prevtx signing for\n"
            "    {\n"
            "     \"txid\":\"id\",                   (string, required) The transaction id\n"
            "     \"vout\":n,                      (numeric, required) The output number\n"
            "     \"scriptPubKey\": \"hex\",         (string, required) script key\n"
            "     \"redeemScript\": \"hex\",         (string, required for P2SH or P2WSH) redeem script\n"
            "     \"amount\": value                (numeric, required) The amount spent\n"
            "   }\n"
            "3. \"address\"                        (string, required) The address of the private key to sign with\n"
            "4. \"sighashtype\"                    (string, optional, default=ALL) The signature hash type. Must be one of\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\"\n"
            "\nResult:\n"
            "The hex encoded signature.\n"
            "\nExamples:\n"
            + HelpExampleCli("createsignaturewithwallet", "\"myhex\" 0 \"myaddress\"")
            + HelpExampleRpc("createsignaturewithwallet", "\"myhex\", 0, \"myaddress\"")
        );

    ObserveSafeMode();

    EnsureWalletIsUnlocked(pwallet);

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ, UniValue::VSTR, UniValue::VSTR}, true);

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, request.params[0].get_str(), true)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    UniValue prevOut = request.params[1].get_obj();

    RPCTypeCheckObj(prevOut,
        {
            {"txid", UniValueType(UniValue::VSTR)},
            {"vout", UniValueType(UniValue::VNUM)},
            {"scriptPubKey", UniValueType(UniValue::VSTR)},
        });

    uint256 txid = ParseHashO(prevOut, "txid");

    int nOut = find_value(prevOut, "vout").get_int();
    if (nOut < 0) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "vout must be positive");
    }

    COutPoint out(txid, nOut);
    std::vector<unsigned char> pkData(ParseHexO(prevOut, "scriptPubKey"));
    CScript scriptRedeem, scriptPubKey(pkData.begin(), pkData.end());

    if (!prevOut.exists("amount"))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "amount is required");
    CAmount nValue = AmountFromValue(prevOut["amount"]);

    if (prevOut.exists("redeemScript"))
    {
        std::vector<unsigned char> redeemData(ParseHexO(prevOut, "redeemScript"));
        scriptRedeem = CScript(redeemData.begin(), redeemData.end());
    };

    CKeyID idSign;
    CTxDestination dest = DecodeDestination(request.params[2].get_str());
    if (!IsValidDestination(dest))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");

    if (dest.type() == typeid(CKeyID))
    {
        idSign = boost::get<CKeyID>(dest);
    } else
    {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unsupported destination type.");
    };

    const UniValue &hashType = request.params[3];
    int nHashType = SIGHASH_ALL;
    if (!hashType.isNull()) {
        static std::map<std::string, int> mapSigHashValues = {
            {std::string("ALL"), int(SIGHASH_ALL)},
            {std::string("ALL|ANYONECANPAY"), int(SIGHASH_ALL|SIGHASH_ANYONECANPAY)},
            {std::string("NONE"), int(SIGHASH_NONE)},
            {std::string("NONE|ANYONECANPAY"), int(SIGHASH_NONE|SIGHASH_ANYONECANPAY)},
            {std::string("SINGLE"), int(SIGHASH_SINGLE)},
            {std::string("SINGLE|ANYONECANPAY"), int(SIGHASH_SINGLE|SIGHASH_ANYONECANPAY)},
        };
        std::string strHashType = hashType.get_str();
        if (mapSigHashValues.count(strHashType)) {
            nHashType = mapSigHashValues[strHashType];
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid sighash param");
        }
    }


    // Sign the transaction
    LOCK2(cs_main, pwallet->cs_wallet);

    std::vector<uint8_t> vchSig;
    unsigned int i;
    for (i = 0; i < mtx.vin.size(); i++) {
        CTxIn& txin = mtx.vin[i];

        if (txin.prevout == out)
        {
            std::vector<uint8_t> vchAmount(8);
            memcpy(&vchAmount[0], &nValue, 8);
            MutableTransactionSignatureCreator creator(pwallet, &mtx, i, vchAmount, nHashType);
            CScript &scriptSig = scriptPubKey.IsPayToScriptHashAny() ? scriptRedeem : scriptPubKey;

            if (!creator.CreateSig(vchSig, idSign, scriptSig, SIGVERSION_BASE))
                throw JSONRPCError(RPC_MISC_ERROR, "CreateSig failed.");

            break;
        };
    };

    if (i >= mtx.vin.size())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "No matching input found.");


    return HexStr(vchSig);
}

UniValue debugwallet(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "debugwallet ( attempt_repair )\n"
            "Detect problems in wallet.\n"
            + HelpRequiringPassphrase(pwallet));

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    bool fAttemptRepair = false;
    if (request.params.size() > 0)
    {
        std::string s = request.params[0].get_str();
        if (nix::IsStringBoolPositive(s))
            fAttemptRepair = true;
    };

    EnsureWalletIsUnlocked(pwallet);

    UniValue result(UniValue::VOBJ);
    UniValue errors(UniValue::VARR);
    UniValue warnings(UniValue::VARR);
    result.pushKV("wallet_name", pwallet->GetName());


    size_t nUnabandonedOrphans = 0;
    size_t nAbandonedOrphans = 0;
    size_t nMapWallet = 0;

    {
        LOCK2(cs_main, pwallet->cs_wallet);

        std::map<uint256, CWalletTx>::const_iterator it;
        for (it = pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); ++it)
        {
            const uint256 &wtxid = it->first;
            const CWalletTx &wtx = it->second;

            nMapWallet++;

        };

        LogPrintf("nUnabandonedOrphans %d\n", nUnabandonedOrphans);
        LogPrintf("nAbandonedOrphans %d\n", nAbandonedOrphans);
        LogPrintf("nMapWallet %d\n", nMapWallet);
        result.pushKV("unabandoned_orphans", (int)nUnabandonedOrphans);

        int64_t rv = 0;
        if (pwallet->CountRecords("sxkm", rv))
            result.pushKV("locked_stealth_outputs", (int)rv);
        else
            result.pushKV("locked_stealth_outputs", "error");

        if (pwallet->CountRecords("lao", rv))
            result.pushKV("locked_blinded_outputs", (int)rv);
        else
            result.pushKV("locked_blinded_outputs", "error");

        // Check for gaps in the hd key chains
        ExtKeyAccountMap::const_iterator itam = pwallet->mapExtAccounts.begin();
        for ( ; itam != pwallet->mapExtAccounts.end(); ++itam)
        {
            CExtKeyAccount *sea = itam->second;
            LogPrintf("Checking account %s\n", sea->GetIDString58());
            for (CStoredExtKey *sek : sea->vExtKeys)
            {
                if (!(sek->nFlags & EAF_ACTIVE)
                    || !(sek->nFlags & EAF_RECEIVE_ON))
                    continue;

                UniValue rva(UniValue::VARR);
                LogPrintf("Checking chain %s\n", sek->GetIDString58());
                uint32_t nGenerated = sek->GetCounter(false);
                LogPrintf("Generated %d\n", nGenerated);

                bool fHardened = false;
                CPubKey newKey;

                for (uint32_t i = 0; i < nGenerated; ++i)
                {
                    uint32_t nChildOut;
                    if (0 != sek->DeriveKey(newKey, i, nChildOut, fHardened))
                        throw JSONRPCError(RPC_WALLET_ERROR, "DeriveKey failed.");

                    if (i != nChildOut)
                        LogPrintf("Warning: %s - DeriveKey skipped key %d, %d.\n", __func__, i, nChildOut);

                    CEKAKey ak;
                    CKeyID idk = newKey.GetID();
                    CPubKey pk;
                    if (!sea->GetPubKey(idk, pk))
                    {
                        UniValue tmp(UniValue::VOBJ);
                        tmp.pushKV("position", (int)i);
                        tmp.pushKV("address", CBitcoinAddress(idk).ToString());

                        if (fAttemptRepair)
                        {
                            uint32_t nChain;
                            if (!sea->GetChainNum(sek, nChain))
                                throw JSONRPCError(RPC_WALLET_ERROR, "GetChainNum failed.");

                            CEKAKey ak(nChain, nChildOut);
                            if (0 != pwallet->ExtKeySaveKey(sea, idk, ak))
                                throw JSONRPCError(RPC_WALLET_ERROR, "ExtKeySaveKey failed.");

                            UniValue b;
                            b.setBool(true);
                            tmp.pushKV("attempt_fix", b);
                        };

                        rva.push_back(tmp);
                    };
                };

                if (rva.size() > 0)
                {
                    UniValue tmp(UniValue::VOBJ);
                    tmp.pushKV("account", sea->GetIDString58());
                    tmp.pushKV("chain", sek->GetIDString58());
                    tmp.pushKV("missing_keys", rva);
                    errors.push_back(tmp);
                };

                // TODO: Check hardened keys, must detect stealth key chain
            };
        };

        {
            CHDWalletDB wdb(pwallet->GetDBHandle(), "r+");
            for (const auto &ri : pwallet->mapRecords)
            {
                const uint256 &txhash = ri.first;
                const CTransactionRecord &rtx = ri.second;

                if (!pwallet->IsTrusted(txhash, rtx.blockHash, rtx.nIndex))
                    continue;

                for (const auto &r : rtx.vout)
                {
                    if ((r.nFlags & ORF_OWNED)
                        && !pwallet->IsSpent(txhash, r.n))
                    {
                        CStoredTransaction stx;
                        if (!wdb.ReadStoredTx(txhash, stx))
                        {
                            UniValue tmp(UniValue::VOBJ);
                            tmp.pushKV("type", "Missing stored txn.");
                            tmp.pushKV("txid", txhash.ToString());
                            tmp.pushKV("n", r.n);
                            errors.push_back(tmp);
                            continue;
                        };

                    };
                };
            };
        }
    }

    result.pushKV("errors", errors);
    result.pushKV("warnings", warnings);

    return result;
};

UniValue walletsettings(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "walletsettings \"setting\" json\n"
            "\nManage wallet settings.\n"
            + HelpRequiringPassphrase(pwallet) +
            "\nchangeaddress {\"address_standard\":}.\n"
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    EnsureWalletIsUnlocked(pwallet);

    UniValue result(UniValue::VOBJ);

    std::string sSetting = request.params[0].get_str();

    if (sSetting == "changeaddress")
    {
        UniValue json;
        UniValue warnings(UniValue::VARR);

        if (request.params.size() == 1)
        {
            if (!pwallet->GetSetting("changeaddress", json))
            {
                result.pushKV(sSetting, "default");
            } else
            {
                result.pushKV(sSetting, json);
            };
            return result;
        };

        if (request.params[1].isObject())
        {
            json = request.params[1].get_obj();

            const std::vector<std::string> &vKeys = json.getKeys();
            if (vKeys.size() < 1)
            {
                if (!pwallet->EraseSetting(sSetting))
                    throw JSONRPCError(RPC_WALLET_ERROR, _("EraseSetting failed."));
                result.pushKV(sSetting, "cleared");
                return result;
            };

            for (const auto &sKey : vKeys)
            {
                if (sKey == "address_standard")
                {
                    if (!json["address_standard"].isStr())
                        throw JSONRPCError(RPC_INVALID_PARAMETER, _("address_standard must be a string."));

                    std::string sAddress = json["address_standard"].get_str();
                    CBitcoinAddress addr(sAddress);
                    if (!addr.IsValid())
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid address_standard.");
                } else
                {
                    warnings.push_back("Unknown key " + sKey);
                };
            };

            json.pushKV("time", GetTime());
            if (!pwallet->SetSetting(sSetting, json))
                throw JSONRPCError(RPC_WALLET_ERROR, _("SetSetting failed."));

            if (warnings.size() > 0)
                result.pushKV("warnings", warnings);
        } else
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Must be json object."));
        };
        result.pushKV(sSetting, json);
    } else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Unknown setting"));
    };

    return result;
};

UniValue transactionblinds(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "transactionblinds \"txnid\"\n"
            "\nShow known blinding factors for transaction.\n"
            + HelpRequiringPassphrase(pwallet) + "\n"
            "\nArguments:\n"
            "1. \"txnid\"                          (string, required) The transaction id\n"
            "\nResult:\n"
            "   {\n"
            "     \"n\":\"hex\",                   (string) The blinding factor for output n, hex encoded\n"
            "   }\n"
            "\nExamples:\n"
            + HelpExampleCli("transactionblinds", "\"txnid\"")
            + HelpExampleRpc("transactionblinds", "\"txnid\"")
        );

    ObserveSafeMode();

    EnsureWalletIsUnlocked(pwallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    MapRecords_t::const_iterator mri = pwallet->mapRecords.find(hash);

    if (mri == pwallet->mapRecords.end())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    //const CTransactionRecord &rtx = mri->second;

    UniValue result(UniValue::VOBJ);
    CStoredTransaction stx;
    if (!CHDWalletDB(pwallet->GetDBHandle()).ReadStoredTx(hash, stx))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No stored data found for txn");

    return result;
};

UniValue derivefromstealthaddress(const JSONRPCRequest &request)
{
    CHDWallet *pwallet = GetHDWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "derivefromstealthaddress \"stealthaddress\"\n"
            "\nDerive a pubkey from a stealth address and random value.\n"
            "\nArguments:\n"
            "1. \"stealthaddress\"                 (string, required) The stealth address\n"
            "\nResult:\n"
            "   {\n"
            "     \"address\":\"base58\",            (string) The derived address\n"
            "     \"pubkey\":\"hex\",                (string) The derived public key\n"
            "     \"ephemeral\":\"hex\",             (string) The ephemeral value\n"
            "   }\n"
            "\nExamples:\n"
            + HelpExampleCli("derivefromstealthaddress", "\"stealthaddress\"")
            + HelpExampleRpc("derivefromstealthaddress", "\"stealthaddress\"")
        );

    ObserveSafeMode();

    CBitcoinAddress addr(request.params[0].get_str());
    if (!addr.IsValidStealthAddress())
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Must input a stealthaddress."));

    CStealthAddress sx = boost::get<CStealthAddress>(addr.Get());


    UniValue result(UniValue::VOBJ);

    CKey sShared, sEphem;
    ec_point pkSendTo;
    sEphem.MakeNewKey(true);
    if (0 != StealthSecret(sEphem, sx.scan_pubkey, sx.spend_pubkey, sShared, pkSendTo))
        throw JSONRPCError(RPC_INTERNAL_ERROR, _("StealthSecret failed, try again."));

    CPubKey pkEphem = sEphem.GetPubKey();
    CPubKey pkDest(pkSendTo);
    CTxDestination dest = GetDestinationForKey(pkDest, OUTPUT_TYPE_LEGACY);

    result.pushKV("address", EncodeDestination(dest));
    result.pushKV("pubkey", HexStr(pkDest));
    result.pushKV("ephemeral", HexStr(pkEphem));


    return result;
};


UniValue generate(const JSONRPCRequest& request)
{
    CHDWallet * const pwallet = GetHDWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            "generate nblocks ( maxtries )\n"
            "\nMine up to nblocks blocks immediately (before the RPC call returns) to an address in the wallet.\n"
            "\nArguments:\n"
            "1. nblocks      (numeric, required) How many blocks are generated immediately.\n"
            "2. maxtries     (numeric, optional) How many iterations to try (default = 1000000).\n"
            "\nResult:\n"
            "[ blockhashes ]     (array) hashes of blocks generated\n"
            "\nExamples:\n"
            "\nGenerate 11 blocks\n"
            + HelpExampleCli("generate", "11")
        );
    }

    int num_generate = request.params[0].get_int();
    uint64_t max_tries = 1000000;
    if (!request.params[1].isNull()) {
        max_tries = request.params[1].get_int();
    }

    CScript coinbase_script;

    pwallet->GetScriptForMining(coinbase_script);

    // If the keypool is exhausted, no script is returned at all.  Catch this.
    //if (!coinbase_script) {
    //    throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    //}

    //throw an error if no script was provided
    if (coinbase_script.empty()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No coinbase script available");
    }

    return generateBlocks(coinbase_script, num_generate, max_tries, true);
}

UniValue generatetoaddress(const JSONRPCRequest& request)
{
    CHDWallet * const pwallet = GetHDWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3)
        throw std::runtime_error(
            "generatetoaddress nblocks address (maxtries)\n"
            "\nMine blocks immediately to a specified address (before the RPC call returns)\n"
            "\nArguments:\n"
            "1. nblocks      (numeric, required) How many blocks are generated immediately.\n"
            "2. address      (string, required) The address to send the newly generated bitcoin to.\n"
            "3. maxtries     (numeric, optional) How many iterations to try (default = 1000000).\n"
            "\nResult:\n"
            "[ blockhashes ]     (array) hashes of blocks generated\n"
            "\nExamples:\n"
            "\nGenerate 11 blocks to myaddress\n"
            + HelpExampleCli("generatetoaddress", "11 \"myaddress\"")
        );

    int nGenerate = request.params[0].get_int();
    uint64_t nMaxTries = 1000000;
    if (!request.params[2].isNull()) {
        nMaxTries = request.params[2].get_int();
    }

    CBitcoinAddress destination(request.params[1].get_str());
    if (!destination.IsValid()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");
    }
    CScript script;
    pwallet->GetScriptForAddress(script, destination);
    return generateBlocks(script, nGenerate, nMaxTries, false);
}

static const CRPCCommand commands[] =
{ //  category              name                                actor (function)                argNames
  //  --------------------- ------------------------            -----------------------         ----------
    { "wallet",             "extkey",                           &extkey,                        {} },
    { "wallet",             "extkeyimportmaster",               &extkeyimportmaster,            {"source","passphrase","save_bip44_root","master_label","account_label","scan_chain_from"} }, // import, set as master, derive account, set default account, force users to run mnemonic new first make them copy the key
    { "wallet",             "extkeygenesisimport",              &extkeygenesisimport,           {"source","passphrase","save_bip44_root","master_label","account_label","scan_chain_from"} },
    { "wallet",             "extkeyaltversion",                 &extkeyaltversion,              {"ext_key"} },
    { "wallet",             "getnewextaddress",                 &getnewextaddress,              {"label","childNo","bech32","hardened"} },
    { "wallet",             "getnewstealthaddress",             &getnewstealthaddress,          {"label","num_prefix_bits","prefix_num","bech32","makeV2"} },
    { "wallet",             "importstealthaddress",             &importstealthaddress,          {"scan_secret","spend_secret","label","num_prefix_bits","prefix_num","bech32"} },
    { "wallet",             "liststealthaddresses",             &liststealthaddresses,          {"show_secrets"} },

    { "wallet",             "scanchain",                        &scanchain,                     {"from_height"} },
    { "wallet",             "reservebalance",                   &reservebalance,                {"enabled","amount"} },
    { "wallet",             "deriverangekeys",                  &deriverangekeys,               {"start","end","key/id","hardened","save","add_to_addressbook","256bithash"} },
    { "wallet",             "clearwallettransactions",          &clearwallettransactions,       {"remove_all"} },

    { "wallet",             "filtertransactions",               &filtertransactions,            {"options"} },
    { "wallet",             "filteraddresses",                  &filteraddresses,               {"offset","count","sort_code"} },
    { "wallet",             "manageaddressbook",                &manageaddressbook,             {"action","address","label","purpose"} },


    { "wallet",             "buildscript",                      &buildscript,                   {"json"} },
    { "wallet",             "createsignaturewithwallet",        &createsignaturewithwallet,     {"hexstring","prevtx","address","sighashtype"} },

    { "wallet",             "debugwallet",                      &debugwallet,                   {"attempt_repair"} },

    { "wallet",             "walletsettings",                   &walletsettings,                {"setting","json"} },

    { "wallet",             "transactionblinds",                &transactionblinds,             {"txnid"} },
    { "wallet",             "derivefromstealthaddress",         &derivefromstealthaddress,      {"stealthaddress"} },
  { "generating",         "generate",                 &generate,                 {"nblocks","maxtries"} },
  { "generating",         "generatetoaddress",      &generatetoaddress,      {"nblocks","address","maxtries"} },


};

void RegisterHDWalletRPCCommands(CRPCTable &t)
{
    if (gArgs.GetBoolArg("-disablewallet", false))
        return;

    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
