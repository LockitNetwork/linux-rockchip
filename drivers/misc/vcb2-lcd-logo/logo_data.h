/* logo_data.h
 *
 * Declares the logo pixel buffer linked in via logo_data.S (assembly incbin).
 * The actual pixel data lives in logo.bin (raw RGB565 big-endian, 240x240).
 *
 * Generate logo.bin with:
 *   python3 img2bin.py your_logo.png logo.bin
 *
 * Or a quick solid-colour test:
 *   python3 img2bin.py --solid 0x001F logo.bin   (blue)
 */

#ifndef LOGO_DATA_H
#define LOGO_DATA_H

extern const unsigned char logo_data[];
extern const unsigned char logo_data_end[];

#define LOGO_DATA_SIZE ((size_t)(logo_data_end - logo_data))

#endif /* LOGO_DATA_H */
