// Copyright 2020-2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_CRYPTSTATEOCB2_H
#define MUMBLE_CRYPTSTATEOCB2_H

#include "CryptState.h"

#include <openssl/evp.h>

#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE_BITS 128
#define AES_KEY_SIZE_BYTES (AES_KEY_SIZE_BITS / 8)


class CryptStateOCB2 : public CryptState {
public:
	CryptStateOCB2();
	~CryptStateOCB2() noexcept override;

	virtual bool isValid() const override;
	virtual void genKey() override;
	virtual bool setKey(const std::string &rkey, const std::string &eiv, const std::string &div) override;
	virtual bool setRawKey(const std::string &rkey) override;
	virtual bool setEncryptIV(const std::string &iv) override;
	virtual bool setDecryptIV(const std::string &iv) override;
	virtual std::string getRawKey() override;
	virtual std::string getEncryptIV() override;
	virtual std::string getDecryptIV() override;

	virtual bool decrypt(const unsigned char *source, unsigned char *dst, unsigned int crypted_length) override;
	virtual bool encrypt(const unsigned char *source, unsigned char *dst, unsigned int plain_length) override;

	bool ocb_encrypt(const unsigned char *plain, unsigned char *encrypted, unsigned int len, const unsigned char *nonce,
					 unsigned char *tag, bool modifyPlainOnXEXStarAttack = true);
	bool ocb_decrypt(const unsigned char *encrypted, unsigned char *plain, unsigned int len, const unsigned char *nonce,
					 unsigned char *tag);

private:
	unsigned char raw_key[AES_KEY_SIZE_BYTES];
	unsigned char encrypt_iv[AES_BLOCK_SIZE];
	unsigned char decrypt_iv[AES_BLOCK_SIZE];
	unsigned char decrypt_history[0x100];

	EVP_CIPHER_CTX *enc_ctx_ocb_enc;
	EVP_CIPHER_CTX *dec_ctx_ocb_enc;
	EVP_CIPHER_CTX *enc_ctx_ocb_dec;
	EVP_CIPHER_CTX *dec_ctx_ocb_dec;
};


#endif // MUMBLE_CRYPTSTATEOCB2_H
