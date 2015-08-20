/*
Copyright (C) 2013-2014 Draios inc.

This file is part of sysdig.

sysdig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

sysdig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with sysdig.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <assert.h>

#include <scap.h>
#include <scap_next.h>
#include "../../../../driver/ppm_events_public.h"
#include "../../driver/ppm_ringbuffer.h"

extern const struct ppm_event_info g_event_info[];

static inline int32_t scap_readbuf_local(scap_device* dev, OUT char** buf, OUT uint32_t* len)
{
	uint32_t ttail;
	uint32_t read_size;

	//
	// Update the tail based on the amount of data read in the *previous* call.
	// Tail is never updated when we serve the data, because we assume that the caller is using
	// the buffer we give to her until she calls us again.
	//
	ttail = dev->m_bufinfo->tail + dev->m_lastreadsize;


	if(ttail >= RING_BUF_SIZE)
	{
		ttail = ttail - RING_BUF_SIZE;
	}
	//
	// Make sure every read of the old buffer is completed before we move the tail and the
	// producer (on another CPU) can start overwriting it.
	// I use this instead of asm(mfence) because it should be portable even on the weirdest
	// CPUs
	//
	__sync_synchronize();
	dev->m_bufinfo->tail = ttail;

	//
	// Read the pointers.
	//
	read_size = get_read_size(dev->m_bufinfo);

	//
	// Remember read_size so we can update the tail at the next call
	//
	dev->m_lastreadsize = read_size;

	//
	// Return the results
	//
	*len = read_size;
	*buf = dev->m_buffer + ttail;

	return SCAP_SUCCESS;
}

size_t g_get_event_size(enum ppm_event_type event_type, uint16_t* lens)
{
	uint32_t j;
	int32_t res = 0;

	for(j = 0; j < g_event_info[event_type].nparams; j++)
	{	
		res += lens[j];
	}

#ifdef PPM_ENABLE_SENTINEL
	return res + j * sizeof(uint16_t) + sizeof(struct ppm_evt_hdr) + sizeof(uint32_t);
#else
	return res + j * sizeof(uint16_t) + sizeof(struct ppm_evt_hdr);
#endif
}

int32_t g_check_integrity(uint32_t* cur_event, char* copy_buffer, int buf_len, OUT uint32_t* nevents)
{
	uint32_t offset = 0;
	*nevents = 0;

	while(buf_len)
	{
#ifdef PPM_ENABLE_SENTINEL
		uint32_t sentinel_begin;
		uint32_t sentinel_end;
#endif
		struct ppm_evt_hdr* hdr;
		size_t event_size;

		if(buf_len < sizeof(struct ppm_evt_hdr))
		{
			fprintf(stderr, "Error: event not on buffer boundary, offset %x, data to read %d\n",
			        offset,
			        buf_len);
			return SCAP_FAILURE;
		}

		hdr = (struct ppm_evt_hdr*)(copy_buffer + offset);

		uint16_t type = hdr->type;
		if(buf_len < sizeof(struct ppm_evt_hdr) + g_event_info[type].nparams * sizeof(uint16_t))
		{
			fprintf(stderr, "Error: event not on buffer boundary, offset %x, data to read %d\n",
			        offset,
			        buf_len);
			return SCAP_FAILURE;
		}

		event_size = g_get_event_size(hdr->type, (uint16_t*)(copy_buffer + offset + sizeof(struct ppm_evt_hdr)));

		if(event_size == -1)
		{
			fprintf(stderr, "Error: unrecognized event %u, cnt %u, offset %x\n",
			        (uint32_t)(hdr->type),
			        (*cur_event == -1)?0:*cur_event,
			        offset);
			return SCAP_FAILURE;
		}

		if(event_size < sizeof(struct ppm_evt_hdr) + g_event_info[hdr->type].nparams * sizeof(uint16_t))
		{
			fprintf(stderr, "Error: event size too short %u, cnt %u, offset %x\n",
			        (unsigned int)event_size,
			        (*cur_event == -1)?0:*cur_event,
			        offset);
			return SCAP_FAILURE;
		}

#ifdef PPM_ENABLE_SENTINEL
		sentinel_begin = ((struct ppm_evt_hdr*)(copy_buffer + offset))->sentinel_begin;
		sentinel_end = *(uint32_t*)(copy_buffer + offset + event_size - sizeof(uint32_t));
		
		if(sentinel_begin != sentinel_end)
		{
			fprintf(stderr, "Error: sentinel begin %d, sentinel end %d, evt_type %u, evt_size %zu, cnt %u, offset %x, remaining %u\n",
			        sentinel_begin,
			        sentinel_end,
			        (uint32_t)hdr->type,
			        event_size,
			        (*cur_event == -1)?0:*cur_event,
			        offset,
			        buf_len);
			return SCAP_FAILURE;
		}

		if(*cur_event == -1)
		{
			*cur_event = sentinel_begin;
		}

		if(sentinel_begin != *cur_event)
		{
			fprintf(stderr, "Error1: sentinel begin %d, sentinel end %d, cnt %u, offset %x, remaining %u\n",
			        sentinel_begin,
			        sentinel_end,
			        *cur_event,
			        offset,
			        buf_len);
			return SCAP_FAILURE;
		}
#endif

		buf_len -= event_size;
		offset += event_size;

		++(*nevents);
		++(*cur_event);
	}

	return 0;
}

int main()
{
	uint32_t j;
	char error[SCAP_LASTERR_SIZE];
	int32_t ret;
	char* buf;
	uint32_t buflen;
	uint32_t cur_evts[256];
	int32_t ndevs;
	uint32_t nloops = 0;
	uint64_t totbytes = 0;
	uint64_t totevents = 0;
	uint64_t devicebytes[256];
	uint64_t deviceevents[256];
	uint64_t oldtotbytes = 0;
	uint64_t oldtotevents = 0;
	uint64_t olddevicebytes[256];
	uint64_t olddeviceevents[256];

	/*
		unsigned long new_mask = 1 << (1);
		sched_setaffinity(0,
			sizeof(unsigned long),
			&new_mask);
	*/

	scap_t* h = scap_open_live(error);
	if(h == NULL)
	{
		fprintf(stderr, "%s\n", error);
		return -1;
	}

	ndevs = scap_get_ndevs(h);

	if(ndevs > sizeof(cur_evts)/sizeof(cur_evts[0]))
	{
		fprintf(stderr, "too many devices %u\n", ndevs);
		return -1;
	}

	for(j = 0; j < ndevs; j++)
	{
		devicebytes[j] = 0;
		deviceevents[j] = 0;
		olddevicebytes[j] = 0;
		olddeviceevents[j] = 0;
	}

	while(1)
	{
		for(j = 0; j < ndevs; j++)
		{
			uint32_t nevents;

			ret = scap_readbuf_local(&(h->m_devs[j]), &buf, &buflen);

			if(ret != SCAP_SUCCESS)
			{
				fprintf(stderr, "%s\n", scap_getlasterr(h));
				scap_close(h);
				return -1;
			}

			cur_evts[j] = -1;

			if(g_check_integrity(&(cur_evts[j]), buf, buflen, &nevents) != SCAP_SUCCESS)
			{
				fprintf(stderr, "Integrity check failure at event %u.\nDumping buffer to dump.bin\n",
				        (cur_evts[j] == -1)?0:cur_evts[j]);

				FILE* f;
				f= fopen("dump.bin", "w");
				fwrite(buf, buflen, 1, f);
				fclose(f);
				exit(-1);
			}

			totbytes += buflen;
			totevents += nevents;
			devicebytes[j] += buflen;
			deviceevents[j] += nevents;

			if(nloops == 1000)
			{
				printf(" %u)bps:%" PRIu64 " totbytes:%" PRIu64 " - evts/s:%" PRIu64 " totevs:%" PRIu64 " \n",
				       j,
				       (devicebytes[j] - olddevicebytes[j]),
				       devicebytes[j],
				       (deviceevents[j] - olddeviceevents[j]),
				       deviceevents[j]);

				olddevicebytes[j] = devicebytes[j];
				olddeviceevents[j] = deviceevents[j];
			}
		}

		//
		// XXX this should check the buffer sizes and sleep only if they are all below a certain
		// threshold.
		//
		usleep(1000);

		if(nloops == 1000)
		{
			scap_stats stats;

			if(scap_get_stats(h, &stats) != SCAP_SUCCESS)
			{
				fprintf(stderr, "%s\n", scap_getlasterr(h));
				scap_close(h);
				return -1;				
			}

			printf("bps:%" PRIu64 " totbytes:%" PRIu64 " - evts/s:%" PRIu64 " totevs:%" PRIu64 " drops:%" PRIu64 "\n",
			       totbytes - oldtotbytes,
			       totbytes,
			       totevents - oldtotevents,
			       totevents,
			       stats.n_drops);

			oldtotbytes = totbytes;
			oldtotevents = totevents;

			nloops = 0;
		}

		nloops++;
	}

	scap_close(h);
	return 0;
}
