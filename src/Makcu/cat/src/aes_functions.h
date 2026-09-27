#ifndef AES_FUNCTIONS_H
#define AES_FUNCTIONS_H

#include <cstdint>

void rot_word(unsigned char* word);
void sub_word(unsigned char* word);
void sub_bytes(unsigned char* state);
void shift_rows(unsigned char* state);
void mix_columns(unsigned char* state);
void key_expansion(const unsigned char* key, unsigned char* expanded_key);
void inv_sub_bytes(unsigned char* state);
void inv_shift_rows(unsigned char* state);
void inv_mix_columns(unsigned char* state);
void aes_encrypt_round(unsigned char* state, const unsigned char* round_key);
void aes_decrypt_round(unsigned char* state, const unsigned char* round_key);
void add_round_key(unsigned char* state, const unsigned char* round_key);
unsigned char gmul(unsigned char a, unsigned char b);

#endif // AES_FUNCTIONS_H