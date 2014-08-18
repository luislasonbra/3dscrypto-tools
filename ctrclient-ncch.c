#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ctrclient.h"
#include "utils.h"
#include "ncch.h"

unsigned int ncchoff = 0;
int noromfs = 0;
ctrclient client;
FILE *finput, *foutput;
ctr_ncchheader ncch_hdr;
u32 mediaunitsize = 0;
unsigned char *buffer;
int use_newncchcrypto = 0;

int write_ncch(u32 section_offset, u32 offset, u32 size, ctr_ncchtypes type, int cryptotype)
{
	unsigned int sz;
	unsigned int chunksize = CHUNKMAXSIZE;
	int i;
	unsigned int curpos = 0;
	unsigned char counter[16];

	if(cryptotype)
	{
		memset(counter, 0, 16);
		for(i=0; i<8; i++)
				counter[i] = ncch_hdr.partitionid[7-i];
			counter[8] = type;

		section_offset>>=4;

		counter[12] = (section_offset >> 24) & 0xff;
		counter[13] = (section_offset >> 16) & 0xff;
		counter[14] = (section_offset >> 8) & 0xff;
		counter[15] = section_offset & 0xff;

		if (!ctrclient_aes_set_ctr(&client, counter))return 1;

		if(cryptotype==1)
		{
			printf("Using regular NCCH crypto keyslot.\n");
			if(!ctrclient_aes_select_key(&client, 0x2c))return 1;
		}
		else if(cryptotype==2)
		{
			printf("Using v7.0 NCCH crypto keyslot.\n");
			if(!ctrclient_aes_select_key(&client, 0x25))return 1;
		}
	}

	fseek(finput, ncchoff + offset, SEEK_SET);
	fseek(foutput, offset, SEEK_SET);

	while(curpos<size)
	{
		if(size - curpos < chunksize)chunksize = size - curpos;

		memset(buffer, 0, chunksize);
		if((sz = fread(buffer, 1, chunksize, finput)) != chunksize)
		{
			if(sz==0)
			{
				printf("Read fail.\n");
				return 1;
			}

			printf("Read fail, only the portion that was read will be decrypted.\n");
			chunksize = sz;
			
		}
		if(cryptotype)printf("Chunk pos %x size %x\n", curpos, chunksize);

		if (cryptotype && !ctrclient_aes_ctr_crypt(&client, buffer, chunksize))
		{
			return 1;
		}

		if(fwrite(buffer, 1, chunksize, foutput) != chunksize)
		{
			printf("Write fail.\n");
			return 1;
		}

		curpos+= chunksize;
	}

	return 0;
}

int write_section(char *section_name, u32 section_offset, u32 media_offset, u32 size, ctr_ncchtypes type, int cryptotype)
{
	if(media_offset==0)
	{
		printf("%s doesn't exist in this NCCH.\n", section_name);
	}
	else
	{
		printf("Processing %s... (section_off=0x%x, offset=0x%x, size=0x%x)\n", section_name, section_offset, media_offset * mediaunitsize, size);
		return write_ncch(section_offset, media_offset * mediaunitsize, size, type, cryptotype);
	}

	return 0;
}

int decrypt_exefs()
{
	int ret=0;
	int type;
	unsigned int pos=0;
	unsigned int size=0;
	unsigned char exefshdr[0x200];
	char str[32];
	char tmpstr[16];

	memset(exefshdr, 0, 0x200);

	if(use_newncchcrypto==0)
	{
		ret = write_section("ExeFS", 0, getle32(ncch_hdr.exefsoffset), getle32(ncch_hdr.exefssize)*mediaunitsize, NCCHTYPE_EXEFS, 1);
		return ret;
	}

	ret = write_section("ExeFS header", 0, getle32(ncch_hdr.exefsoffset), 0x200, NCCHTYPE_EXEFS, 1);
	if(ret)return ret;

	memcpy(exefshdr, buffer, 0x200);

	for(pos=0; pos<0xa0; pos+=0x10)
	{
		if(getle32(&exefshdr[pos])==0)continue;

		memset(str, 0, 32);
		memset(tmpstr, 0, 16);
		memcpy(tmpstr, (char*)&exefshdr[pos], 8);
		snprintf(str, 31, "ExeFS:/%s", tmpstr);

		type = 1;
		if(use_newncchcrypto)
		{
			type = 2;
			if(strncmp(tmpstr, "icon", 4)==0 || strncmp(tmpstr, "banner", 6)==0)type = 1;
		}

		size = (getle32(&exefshdr[pos+12]) + 0xf) & ~0xf;

		ret = write_section(str, getle32(&exefshdr[pos+8]) + 0x200, getle32(ncch_hdr.exefsoffset) + (getle32(&exefshdr[pos+8])/mediaunitsize) + 1, size, NCCHTYPE_EXEFS, type);
		if(ret)return ret;
	}

	return 0;
}

int decrypt_ncch()
{
	int ret;

	ret = write_ncch(0, 0, 0x200, 0, 0);
	if(ret)return ret;

	if(getle32(ncch_hdr.extendedheadersize))
	{
		printf("Processing exheader...\n");
		ret = write_ncch(0, 0x200, 0x800, NCCHTYPE_EXHEADER, 1);
		if(ret)return ret;
	}
	else
	{
		printf("Exheader doesn't exist in this NCCH.\n");
	}

	ret = write_section("Plain-section", 0, getle32(ncch_hdr.plainregionoffset), getle32(ncch_hdr.plainregionsize)*mediaunitsize, 0, 0);
	if(ret)return ret;

	ret = decrypt_exefs();
	if(ret)return ret;
	
	if(!noromfs)
	{
		ret = write_section("RomFS", 0, getle32(ncch_hdr.romfsoffset), getle32(ncch_hdr.romfssize)*mediaunitsize, NCCHTYPE_ROMFS, 1 + use_newncchcrypto);
		if(ret)return ret;
	}

	return 0;
}

int run_ctrtool(char *ncchfn, char *prefix)
{
	int ret;
	char *home;
	char keypath[256];
	char romfs_cmd[256];
	char sys_cmd[1024];

	memset(keypath, 0, 256);
	memset(romfs_cmd, 0, 256);
	memset(sys_cmd, 0, 1024);
	home = getenv("HOME");

	if(home)
	{
		snprintf(keypath, 255, "%s/.3ds/keys.xml", home);
	}
	else
	{
		strncpy(keypath, "keys.xml", 255);
	}

	printf("Running ctrtool with prefix %s and ncch %s...\n", prefix, ncchfn);

	if(!noromfs)snprintf(romfs_cmd, 255, "--romfs=%s.romfs", prefix);
	snprintf(sys_cmd, 1023, "ctrtool -v --verify -p --keyset=%s --exefsdir=%s_exefs %s %s > %s.info", keypath, prefix, romfs_cmd, ncchfn, prefix);
	ret = system(sys_cmd);
	if(ret!=0)return ret;

	if(noromfs)return 0;

	memset(sys_cmd, 0, 256);
	snprintf(sys_cmd, 1023, "ctrtool -v --verify -p --keyset=%s --romfsdir=%s_romfs %s.romfs > %s.romfs_info", keypath, prefix, prefix, prefix);
	return system(sys_cmd);
}

int main(int argc, char *argv[])
{
	int ret;
	int argi;
	u16 version;

	char infn[256];
	char outfn[256];
	char ctrtool_prefix[256];
	char serveradr[256];

	if(argc==1)
	{
		printf("ctrclient-ncch by yellows8\n");
		printf("Decrypt/encrypt a retail secure-key NCCH, options:\n");
		printf("--serveradr=<addr> Use the specified server address instead of the default address.\n");
		printf("--input=<path> Input path for secure key NCCH\n");
		printf("--output=<path> Output path for decrypted NCCH\n");
		printf("--noromfs Skip decrypting the RomFS\n");
		printf("--ctrtoolprefix=<prefix> Run ctrtool with the decrypted NCCH, with the specified prefix for ExeFS, RomFS, and ctrtool stdout file redirect\n");
		printf("--ncchoff=<hexoffset> Base offset for the NCCH in the input\n");
		return 0;
	}

	memset(serveradr, 0, 256);

	memset(infn, 0, 256);
	memset(outfn, 0, 256);
	memset(ctrtool_prefix, 0, 256);

	for(argi=1; argi<argc; argi++)
	{
		if(strncmp(argv[argi], "--serveradr=", 12)==0)strncpy(serveradr, &argv[argi][12], 255);
		if(strncmp(argv[argi], "--input=", 8)==0)strncpy(infn, &argv[argi][8], 255);
		if(strncmp(argv[argi], "--output=", 9)==0)strncpy(outfn, &argv[argi][9], 255);
		if(strncmp(argv[argi], "--ctrtoolprefix=", 16)==0)strncpy(ctrtool_prefix, &argv[argi][16], 255);
		if(strncmp(argv[argi], "--noromfs", 9)==0)noromfs = 1;
		if(strncmp(argv[argi], "--ncchoff=", 10)==0)sscanf(&argv[argi][10], "%x", &ncchoff);
	}

	if(infn[0]==0 || outfn[0]==0 || serveradr[0]==0)return 1;

	buffer = (unsigned char*)malloc(CHUNKMAXSIZE);
	if(buffer==NULL)return 1;
	memset(buffer, 0, CHUNKMAXSIZE);

	finput = fopen(infn, "rb");
	foutput = fopen(outfn, "wb");
	if(finput==NULL || foutput==NULL)
	{
		free(buffer);
		if(finput)fclose(finput);
		if(foutput)fclose(foutput);
		return 1;
	}
	fseek(finput, ncchoff, SEEK_SET);

	if(fread(&ncch_hdr, 1, sizeof(ctr_ncchheader), finput) != sizeof(ctr_ncchheader))
	{
		printf("read fail\n");

		free(buffer);
		fclose(finput);
		fclose(foutput);
		return 1;
	}

	if (getle32(ncch_hdr.magic) != 0x4843434E)
	{
		printf("NCCH magic is invalid.\n");

		free(buffer);
		fclose(finput);
		fclose(foutput);
		return 1;
	}

	version = getle16(ncch_hdr.version);
	if(version!=2 && version!=0)
	{
		printf("NCCH version is invalid, version is %x but only versions 2 and 0 are supported.\n", version);
		
		free(buffer);
		fclose(finput);
		fclose(foutput);
		return 1;
	}

	use_newncchcrypto = 0;
	if(ncch_hdr.flags[3])
	{
		use_newncchcrypto = 1;
		printf("This NCCH uses the v7.0 NCCH crypto, the sections using that will be decrypted with the seperate keyslot needed for that.\n");
	}

	mediaunitsize = 1 << (ncch_hdr.flags[6] + 9);

	ctrclient_init();
	if (0 == ctrclient_connect(&client, serveradr, "8333"))
	{
		free(buffer);
		fclose(finput);
		fclose(foutput);
		exit(1);
	}


	if(!ctrclient_aes_set_ykey(&client, 0x2c, ncch_hdr.signature))
	{
		free(buffer);
		fclose(finput);
		fclose(foutput);
		return 1;
	}

	if(use_newncchcrypto)
	{
		if(!ctrclient_aes_set_ykey(&client, 0x25, ncch_hdr.signature))
		{
			free(buffer);
			fclose(finput);
			fclose(foutput);
			return 1;
		}
	}

	ret = decrypt_ncch();
	ctrclient_disconnect(&client);
	if(ret==0)printf("Done\n");

	free(buffer);
	fclose(finput);
	fclose(foutput);
	if(ret!=0)return ret;

	if(ctrtool_prefix[0])
	{
		ret = run_ctrtool(outfn, ctrtool_prefix);
		if(ret!=0)return ret;
	}

    	return 0;
}

