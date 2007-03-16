#include "940shared.h"

static _940_data_t *shared_data = (_940_data_t *)   0x00100000;
static _940_ctl_t  *shared_ctl  = (_940_ctl_t *)    0x00200000;
static unsigned char *mp3_data  = (unsigned char *) 0x01000000;
YM2612 *ym2612_940;

// from init.s
int  wait_get_job(int oldjob);
void spend_cycles(int c);
void dcache_clean(void);
void dcache_clean_flush(void);
void drain_wb(void);
// this should help to resolve race confition where shared var
// is changed by other core just before we update it
void set_if_not_changed(int *val, int oldval, int newval);

//	asm volatile ("mov r0, #0" ::: "r0");
//	asm volatile ("mcr p15, 0, r0, c7, c6,  0" ::: "r0"); /* flush dcache */
//	asm volatile ("mcr p15, 0, r0, c7, c10, 4" ::: "r0"); /* drain write buffer */


static void mp3_decode(void)
{
	int mp3_offs = shared_ctl->mp3_offs;
	unsigned char *readPtr = mp3_data + mp3_offs;
	int bytesLeft = shared_ctl->mp3_len - mp3_offs;
	int offset; // frame offset from readPtr
	int err;

	if (bytesLeft <= 0) return; // EOF, nothing to do

	offset = MP3FindSyncWord(readPtr, bytesLeft);
	if (offset < 0) {
		set_if_not_changed(&shared_ctl->mp3_offs, mp3_offs, shared_ctl->mp3_len);
		return; // EOF
	}
	readPtr += offset;
	bytesLeft -= offset;

	err = MP3Decode(shared_data->mp3dec, &readPtr, &bytesLeft,
			shared_data->mp3_buffer[shared_ctl->mp3_buffsel], 0);
	if (err) {
		if (err == ERR_MP3_INDATA_UNDERFLOW) {
			set_if_not_changed(&shared_ctl->mp3_offs, mp3_offs, shared_ctl->mp3_len);
			return;
		} else if (err <= -6 && err >= -12) {
			// ERR_MP3_INVALID_FRAMEHEADER, ERR_MP3_INVALID_*
			// just try to skip the offending frame..
			readPtr++;
		}
		shared_ctl->mp3_errors++;
		shared_ctl->mp3_lasterr = err;
	}
	set_if_not_changed(&shared_ctl->mp3_offs, mp3_offs, readPtr - mp3_data);
}


void Main940(void)
{
	int *ym_buffer = shared_data->ym_buffer;
	int job = 0;
	ym2612_940 = &shared_data->ym2612;


	for (;;)
	{
		job = wait_get_job(job);

		shared_ctl->lastjob = job;

		switch (job)
		{
			case JOB940_INITALL:
				/* ym2612 */
				shared_ctl->writebuff0[0] = shared_ctl->writebuff1[0] = 0xffff;
				YM2612Init_(shared_ctl->baseclock, shared_ctl->rate);
				/* Helix mp3 decoder */
				shared_data->mp3dec = MP3InitDecoder();
				break;

			case JOB940_INVALIDATE_DCACHE:
				drain_wb();
				dcache_clean_flush();
				break;

			case JOB940_YM2612RESETCHIP:
				YM2612ResetChip_();
				break;

			case JOB940_PICOSTATELOAD:
				YM2612PicoStateLoad_();
				break;

			case JOB940_YM2612UPDATEONE: {
				int i, dw, *wbuff;
				if (shared_ctl->writebuffsel == 1) {
					wbuff = (int *) shared_ctl->writebuff1;
				} else {
					wbuff = (int *) shared_ctl->writebuff0;
				}

				/* playback all writes */
				for (i = 2048/2; i > 0; i--) {
					UINT16 d;
					dw = *wbuff++;
					d = dw;
					if (d == 0xffff) break;
					YM2612Write_(d >> 8, d);
					d = (dw>>16);
					if (d == 0xffff) break;
					YM2612Write_(d >> 8, d);
				}

				shared_ctl->ym_active_chs =
					YM2612UpdateOne_(ym_buffer, shared_ctl->length, shared_ctl->stereo, 1);
				break;
			}

			case JOB940_MP3DECODE:
				mp3_decode();
				break;
		}

		shared_ctl->loopc++;
		dcache_clean();
	}
}
