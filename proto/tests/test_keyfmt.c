/* Key file recognition (proto/src/keyfmt.c). SPDX-License-Identifier: GPL-3.0-only
 *
 * No private keys are committed: the public lines below come from keys that
 * were deleted after `ssh-keygen -lf`, and the private-key files are synthetic
 * containers with the right structure and random contents. */
#include "proto/keyfmt.h"
#include "tt.h"

#include <stdio.h>
#include <string.h>

static const char ED_PUB[] =
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIKmbNVUx4KjFZz5Wy6q9UbOAJmjohi5fktxyWWAjI2cl fixture";
static const char ED_FP[] = "SHA256:hNUjotZMZK2WpL3WDqUZZeUsLhngno+bZJ8NIDO+9sA";
static const char EC_PUB[] =
    "ecdsa-sha2-nistp384 AAAAE2VjZHNhLXNoYTItbmlzdHAzODQAAAAIbmlzdHAzODQAAABhBG5fTTcv3rOtrnYrqr8iJ5/7lP5Qgloy8HgqfaSfF197l7pIsWEJmPvdQg7KRt7PZ87s1W6lBhBCuGzcEdWl6lmWBfsiPytJO49aLi71kHTVSZpHDA93Wvalnr73mB1+Fg== fixture";
static const char EC_FP[] = "SHA256:H2tu6g2v2UKUC9sJYovCw32GVQD+18Ow7mAWScpCYd4";
static const char RSA_PUB[] =
    "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQCZlRl2a7OxCW4Ilzay0bbHsWXOPb74GprKAYdbLwr+EJt/BFkHrxLzAaxQ7xsTtIobqqvN3aUDi0EetUdNaembN4Xh43+OAkS0ppVnvTZ+M/iJwDXQafQd+Ra282/t2dMTiH0nrnV/EpYCEgwbXOoFYsyvwvKlWZhp1VccbVyLG4xghGkURW1WYVWNHUmmokKUPpwvHex1YJSmzfE228PW4qJMjn0FtIWrPiWHUIMd1xEWyAwaMJviLTLa4JYlw9mCD6xVGELQohllzALBk2yUaEwmn+K7+Kdbr05bCXDckD4HOBy+cYYHHx0B+kEuckEn/DwKgCD5wYTlcPP3c2dmTx3i4JrdygMJQM2D0+YZX9b5Zy9JC+/NRSI6Bjb5MFXo18X92ezXMsLv/SavGC86W8jGosPRfZkKU/b58SKGTFgBCCinUp4w+Getgw+Ffm9fFmFKs4k3ZR++eYNbznCncvMUJ7p4RR6fgGJJPXYxo1qx1lCT8PwFOdwK2UIo6D8= fixture";
static const char RSA_FP[] = "SHA256:nlssYIxdiseQSHOJnMVxbVdvtFE9xQvW1MzyXyEql7w";

static uint8_t scratch[KEYFMT_SCRATCH];
static char text[8192];

static void test_sha256(void) {
    KeyfmtSha256 s;
    uint8_t d[32];
    char hex[65];
    keyfmt_sha256_init(&s);
    keyfmt_sha256_update(&s, "abc", 3);
    keyfmt_sha256_final(&s, d);
    for(int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    TT_EQ_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* two blocks, fed in odd pieces */
    static const char m[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    keyfmt_sha256_init(&s);
    for(size_t i = 0; i < sizeof(m) - 1; i += 5)
        keyfmt_sha256_update(&s, m + i, sizeof(m) - 1 - i < 5 ? sizeof(m) - 1 - i : 5);
    keyfmt_sha256_final(&s, d);
    for(int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    TT_EQ_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_base64(void) {
    char out[16];
    uint8_t back[16];
    TT_EQ_INT(keyfmt_b64_encode((const uint8_t*)"foob", 4, true, out, sizeof(out)), 8);
    TT_EQ_STR(out, "Zm9vYg==");
    TT_EQ_INT(keyfmt_b64_encode((const uint8_t*)"foob", 4, false, out, sizeof(out)), 6);
    TT_EQ_STR(out, "Zm9vYg");
    TT_EQ_INT(
        keyfmt_b64_encode((const uint8_t*)"foobar", 6, true, out, 8), 0); /* no room for the NUL */
    TT_EQ_INT(keyfmt_b64_decode("Zm9v\r\nYmFy", 10, back, sizeof(back)), 6);
    TT_CHECK(memcmp(back, "foobar", 6) == 0);
    TT_EQ_INT(keyfmt_b64_decode("Zm9vYg==", 8, back, sizeof(back)), 4);
    TT_EQ_INT(keyfmt_b64_decode("Zm9vYmFy", 8, back, 3), 3); /* stops when full */
}

static void
    check_pub(const char* line, const char* fp, KeyType type, unsigned bits, const char* label) {
    uint8_t blob[1100];
    size_t n = 0;
    TT_CHECK(keyfmt_parse_pub_line(line, strlen(line), blob, sizeof(blob), &n));
    char f[KEYFMT_FP_SIZE];
    keyfmt_fingerprint(blob, n, f);
    TT_EQ_STR(f, fp);
    KeyInfo ki = {0};
    TT_CHECK(keyfmt_blob_info(blob, n, &ki));
    TT_EQ_INT(ki.type, type);
    TT_EQ_INT(ki.bits, bits);
    char l[KEYFMT_LABEL_SIZE];
    keyfmt_label(&ki, l);
    TT_EQ_STR(l, label);
    char again[1100];
    size_t m = keyfmt_pub_line(blob, n, "fixture", again, sizeof(again));
    TT_EQ_INT(m, strlen(line));
    TT_EQ_STR(again, line);
}

static void test_public_lines(void) {
    check_pub(ED_PUB, ED_FP, KeyTypeEd25519, 256, "Ed25519");
    check_pub(EC_PUB, EC_FP, KeyTypeEcdsa, 384, "ECDSA P-384");
    check_pub(RSA_PUB, RSA_FP, KeyTypeRsa, 3072, "RSA 3072");
    uint8_t blob[64];
    size_t n;
    TT_CHECK(!keyfmt_parse_pub_line("ssh-ed25519", 11, blob, sizeof(blob), &n));
    TT_CHECK(
        !keyfmt_parse_pub_line(RSA_PUB, strlen(RSA_PUB), blob, sizeof(blob), &n)); /* too big */
    /* the type word must match the blob */
    char lie[sizeof(ED_PUB) + 8];
    snprintf(lie, sizeof(lie), "ssh-rsa %s", ED_PUB + 12);
    TT_CHECK(!keyfmt_parse_pub_line(lie, strlen(lie), blob, sizeof(blob), &n));
}

/* ---- synthetic private key files ---- */

static size_t put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return 4;
}

static size_t put_str(uint8_t* p, const void* s, size_t n) {
    put_u32(p, (uint32_t)n);
    memcpy(p + 4, s, n);
    return 4 + n;
}

static void pem(const char* label, const char* headers, const uint8_t* der, size_t n) {
    static char b64[6000];
    keyfmt_b64_encode(der, n, true, b64, sizeof(b64));
    size_t o = (size_t)snprintf(text, sizeof(text), "-----BEGIN %s-----\n%s", label, headers);
    for(size_t i = 0; b64[i]; i += 70)
        o += (size_t)snprintf(text + o, sizeof(text) - o, "%.70s\n", b64 + i);
    snprintf(text + o, sizeof(text) - o, "-----END %s-----\n", label);
}

/* openssh-key-v1 container around a real public key, private part random. */
static void openssh(const char* pub_line, const char* cipher) {
    uint8_t blob[1100], der[3000];
    size_t bn = 0, o = 0;
    keyfmt_parse_pub_line(pub_line, strlen(pub_line), blob, sizeof(blob), &bn);
    memcpy(der, "openssh-key-v1", 15);
    o = 15;
    o += put_str(der + o, cipher, strlen(cipher));
    o += put_str(
        der + o, strcmp(cipher, "none") ? "bcrypt" : "none", strcmp(cipher, "none") ? 6 : 4);
    o += put_str(der + o, "", 0);
    o += put_u32(der + o, 1);
    o += put_str(der + o, blob, bn);
    uint8_t priv[400];
    for(size_t i = 0; i < sizeof(priv); i++)
        priv[i] = (uint8_t)(i * 37 + 11);
    o += put_str(der + o, priv, sizeof(priv));
    pem("OPENSSH PRIVATE KEY", "", der, o);
}

static KeyInfo sniff_text(void) {
    KeyInfo ki;
    size_t n = strlen(text);
    keyfmt_sniff(
        text, n < KEYFMT_SNIFF_BYTES ? n : KEYFMT_SNIFF_BYTES, &ki, scratch, sizeof(scratch));
    return ki;
}

static void test_openssh(void) {
    openssh(ED_PUB, "none");
    KeyInfo ki = sniff_text();
    TT_EQ_INT(ki.fmt, KeyFmtOpenSsh);
    TT_EQ_INT(ki.type, KeyTypeEd25519);
    TT_CHECK(!ki.encrypted);
    TT_CHECK(ki.pub != NULL);
    char fp[KEYFMT_FP_SIZE];
    keyfmt_fingerprint(ki.pub, ki.pub_len, fp);
    TT_EQ_STR(fp, ED_FP); /* the public section gives the fingerprint without decrypting */
    TT_CHECK(keyfmt_problem(&ki) == NULL);

    openssh(RSA_PUB, "aes256-ctr");
    ki = sniff_text();
    TT_EQ_INT(ki.type, KeyTypeRsa);
    TT_EQ_INT(ki.bits, 3072);
    TT_CHECK(ki.encrypted);
    keyfmt_fingerprint(ki.pub, ki.pub_len, fp);
    TT_EQ_STR(fp, RSA_FP);

    openssh(EC_PUB, "aes256-ctr");
    ki = sniff_text();
    TT_EQ_INT(ki.bits, 384);

    /* damaged: magic wrong */
    openssh(ED_PUB, "none");
    char* body = strchr(text, '\n') + 1;
    body[0] = 'A';
    body[1] = 'A';
    ki = sniff_text();
    TT_EQ_INT(ki.fmt, KeyFmtOpenSsh);
    TT_CHECK(keyfmt_problem(&ki) != NULL);
}

static size_t der_hdr(uint8_t* p, uint8_t tag, size_t len) {
    p[0] = tag;
    if(len < 128) {
        p[1] = (uint8_t)len;
        return 2;
    }
    p[1] = 0x82;
    p[2] = (uint8_t)(len >> 8);
    p[3] = (uint8_t)len;
    return 4;
}

/* RSAPrivateKey with a modulus of `bits` bits (random, not a real key) */
static size_t rsa_der(uint8_t* out, unsigned bits) {
    uint8_t body[1200];
    size_t o = 0;
    o += der_hdr(body + o, 0x02, 1);
    body[o++] = 0;
    size_t mlen = bits / 8 + 1;
    o += der_hdr(body + o, 0x02, mlen);
    body[o++] = 0;
    body[o++] = 0x80 | 0x35;
    for(size_t i = 2; i < mlen; i++)
        body[o++] = (uint8_t)(i * 13);
    o += der_hdr(body + o, 0x02, 3);
    body[o++] = 1;
    body[o++] = 0;
    body[o++] = 1;
    size_t h = der_hdr(out, 0x30, o);
    memcpy(out + h, body, o);
    return h + o;
}

static void test_pem(void) {
    uint8_t der[1400];
    size_t n = rsa_der(der, 2048);
    pem("RSA PRIVATE KEY", "", der, n);
    KeyInfo ki = sniff_text();
    TT_EQ_INT(ki.fmt, KeyFmtPemRsa);
    TT_EQ_INT(ki.type, KeyTypeRsa);
    TT_EQ_INT(ki.bits, 2048);
    TT_CHECK(keyfmt_problem(&ki) == NULL);

    n = rsa_der(der, 1024);
    pem("RSA PRIVATE KEY", "", der, n);
    ki = sniff_text();
    TT_EQ_INT(ki.bits, 1024);
    TT_EQ_STR(keyfmt_problem(&ki), "RSA key too short (needs 2048+ bits)");

    pem("RSA PRIVATE KEY",
        "Proc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,00112233445566778899AABBCCDDEEFF\n\n",
        der,
        n);
    ki = sniff_text();
    TT_CHECK(ki.encrypted);
    TT_EQ_INT(ki.type, KeyTypeRsa);
    TT_EQ_INT(ki.bits, 0); /* not known before decryption */
    TT_CHECK(keyfmt_problem(&ki) == NULL);

    /* SEC1 EC key with a 48-byte scalar = P-384 */
    uint8_t ec[64] = {0x30, 0x35, 0x02, 0x01, 0x01, 0x04, 0x30};
    pem("EC PRIVATE KEY", "", ec, 7 + 48);
    ki = sniff_text();
    TT_EQ_INT(ki.fmt, KeyFmtPemEc);
    TT_EQ_INT(ki.bits, 384);

    /* PKCS#8 ed25519 */
    static const uint8_t p8[] = {0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b,
                                 0x65, 0x70, 0x04, 0x22, 0x04, 0x20, 1,    2,    3,    4,
                                 5,    6,    7,    8,    9,    10,   11,   12,   13,   14,
                                 15,   16,   17,   18,   19,   20,   21,   22,   23,   24,
                                 25,   26,   27,   28,   29,   30,   31,   32};
    pem("PRIVATE KEY", "", p8, sizeof(p8));
    ki = sniff_text();
    TT_EQ_INT(ki.fmt, KeyFmtPkcs8);
    TT_EQ_INT(ki.type, KeyTypeEd25519);
    char l[KEYFMT_LABEL_SIZE];
    keyfmt_label(&ki, l);
    TT_EQ_STR(l, "Ed25519");

    /* PKCS#8 P-256 */
    static const uint8_t p8ec[] = {0x30, 0x20, 0x02, 0x01, 0x00, 0x30, 0x13, 0x06,
                                   0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
                                   0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03,
                                   0x01, 0x07, 0x04, 0x04, 0,    0,    0,    0};
    pem("PRIVATE KEY", "", p8ec, sizeof(p8ec));
    ki = sniff_text();
    TT_EQ_INT(ki.type, KeyTypeEcdsa);
    TT_EQ_INT(ki.bits, 256);

    pem("ENCRYPTED PRIVATE KEY", "", p8, sizeof(p8));
    ki = sniff_text();
    TT_EQ_INT(ki.fmt, KeyFmtPkcs8Enc);
    TT_CHECK(ki.encrypted);
    TT_CHECK(keyfmt_problem(&ki) == NULL);
}

static void test_rejected(void) {
    struct {
        const char* text;
        const char* problem;
    } cases[] = {
        {ED_PUB, "This is a public key; copy the private key file too"},
        {"---- BEGIN SSH2 PUBLIC KEY ----\nAAAA\n",
         "This is a public key; copy the private key file too"},
        {"-----BEGIN PUBLIC KEY-----\nMCow\n",
         "This is a public key; copy the private key file too"},
        {"-----BEGIN DSA PRIVATE KEY-----\nMIIB\n-----END DSA PRIVATE KEY-----\n",
         "DSA keys are not supported"},
        {"PuTTY-User-Key-File-3: ssh-ed25519\n", "PuTTY key: export it as OpenSSH first"},
        {"hello world\n", "Not a private key file"},
        {"", "Not a private key file"},
    };
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        KeyInfo ki;
        keyfmt_sniff(cases[i].text, strlen(cases[i].text), &ki, scratch, sizeof(scratch));
        TT_EQ_STR(keyfmt_problem(&ki), cases[i].problem);
    }
    /* an OpenSSH-format DSA key */
    static const char dss[] = "ssh-dss AAAAB3NzaC1kc3MAAAABAQ== x";
    uint8_t blob[64];
    size_t n = 0;
    TT_CHECK(keyfmt_parse_pub_line(dss, strlen(dss), blob, sizeof(blob), &n));
    KeyInfo ki = {.fmt = KeyFmtOpenSsh};
    TT_CHECK(keyfmt_blob_info(blob, n, &ki));
    TT_EQ_STR(keyfmt_problem(&ki), "DSA keys are not supported");
}

static void test_file_id(void) {
    uint8_t d[32] = {0};
    char id[KEYFMT_FP_SIZE + 5];
    keyfmt_file_id(d, id);
    TT_EQ_STR(id, "file:SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
}

/* Random text never crashes the sniffer. */
static void test_fuzz(void) {
    tt_seed(9);
    static const char* const heads[] = {
        "-----BEGIN OPENSSH PRIVATE KEY-----\n",
        "-----BEGIN RSA PRIVATE KEY-----\n",
        "-----BEGIN PRIVATE KEY-----\n",
        "-----BEGIN EC PRIVATE KEY-----\n"};
    for(int it = 0; it < 4000; it++) {
        size_t o = (size_t)snprintf(text, sizeof(text), "%s", heads[it % 4]);
        size_t n = tt_rand() % 1500;
        static const char alpha[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/AAAA\n";
        for(size_t i = 0; i < n; i++)
            text[o + i] = alpha[tt_rand() % (sizeof(alpha) - 1)];
        KeyInfo ki;
        keyfmt_sniff(text, o + n, &ki, scratch, sizeof(scratch));
        char l[KEYFMT_LABEL_SIZE];
        keyfmt_label(&ki, l);
        (void)keyfmt_problem(&ki);
    }
    TT_CHECK(1);
}

int main(void) {
    TT_RUN(test_sha256);
    TT_RUN(test_base64);
    TT_RUN(test_public_lines);
    TT_RUN(test_openssh);
    TT_RUN(test_pem);
    TT_RUN(test_rejected);
    TT_RUN(test_file_id);
    TT_RUN(test_fuzz);
    TT_END();
}
