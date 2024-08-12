// Copyright (c) 2024 Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef PDB_H
#define PDB_H

// https://github.com/microsoft/microsoft-pdb/tree/master/PDB

////////////////////////////////
//~ PDB Format Types

typedef U32 PDB_Version;
enum
{
  PDB_Version_VC2      = 19941610,
  PDB_Version_VC4      = 19950623,
  PDB_Version_VC41     = 19950814,
  PDB_Version_VC50     = 19960307,
  PDB_Version_VC98     = 19970604,
  PDB_Version_VC70_DEP = 19990604,
  PDB_Version_VC70     = 20000404,
  PDB_Version_VC80     = 20030901,
  PDB_Version_VC110    = 20091201,
  PDB_Version_VC140    = 20140508
};

typedef U16 PDB_ModIndex;
typedef U32 PDB_StringIndex;

typedef enum PDB_FixedStream
{
  PDB_FixedStream_PdbInfo = 1,
  PDB_FixedStream_Tpi = 2,
  PDB_FixedStream_Dbi = 3,
  PDB_FixedStream_Ipi = 4
} PDB_FixedStream;

typedef enum PDB_NamedStream
{
  PDB_NamedStream_HEADER_BLOCK,
  PDB_NamedStream_STRTABLE,
  PDB_NamedStream_LINK_INFO,
  PDB_NamedStream_COUNT
} PDB_NamedStream;

typedef struct PDB_InfoHeader
{
  PDB_Version version;
  U32 time;
  U32 age;
} PDB_InfoHeader;

enum
{
  PDB_StrtblHeader_MAGIC = 0xEFFEEFFE
};

typedef struct PDB_StrtblHeader
{
  U32 magic;
  U32 version;
} PDB_StrtblHeader;

////////////////////////////////
//~ PDB Format DBI Types

typedef U32 PDB_DbiStream;
enum
{
  PDB_DbiStream_FPO,
  PDB_DbiStream_EXCEPTION,
  PDB_DbiStream_FIXUP,
  PDB_DbiStream_OMAP_TO_SRC,
  PDB_DbiStream_OMAP_FROM_SRC,
  PDB_DbiStream_SECTION_HEADER,
  PDB_DbiStream_TOKEN_RDI_MAP,
  PDB_DbiStream_XDATA,
  PDB_DbiStream_PDATA,
  PDB_DbiStream_NEW_FPO,
  PDB_DbiStream_SECTION_HEADER_ORIG,
  PDB_DbiStream_COUNT
};

typedef U32 PDB_DbiHeaderSignature;
enum
{
  PDB_DbiHeaderSignature_V1 = 0xFFFFFFFF
};

typedef U32 PDB_DbiVersion;
enum
{
  PDB_DbiVersion_41  =   930803,
  PDB_DbiVersion_50  = 19960307,
  PDB_DbiVersion_60  = 19970606,
  PDB_DbiVersion_70  = 19990903,
  PDB_DbiVersion_110 = 20091201,
};

typedef U16 PDB_DbiBuildNumber;
#define PDB_DbiBuildNumberNewFormatFlag 0x8000
#define PDB_DbiBuildNumberMinor(bn) ((bn)&0xFF)
#define PDB_DbiBuildNumberMajor(bn) (((bn) >> 8)&0x7F)
#define PDB_DbiBuildNumberNewFormat(bn) (!!((bn)&PDB_DbiBuildNumberNewFormatFlag))
#define PDB_DbiBuildNumber(maj, min) \
(PDB_DbiBuildNumberNewFormatFlag | ((min)&0xFF) | (((maj)&0x7F) << 16))

typedef U16 PDB_DbiHeaderFlags;
enum
{
  PDB_DbiHeaderFlag_Incremental = 0x1,
  PDB_DbiHeaderFlag_Stripped    = 0x2,
  PDB_DbiHeaderFlag_CTypes      = 0x4
};

typedef struct PDB_DbiHeader
{
  PDB_DbiHeaderSignature sig;
  PDB_DbiVersion version;
  U32 age;
  MSF_StreamNumber gsi_sn;
  PDB_DbiBuildNumber build_number;
  
  MSF_StreamNumber psi_sn;
  U16 pdb_version;
  
  MSF_StreamNumber sym_sn;
  U16 pdb_version2;
  
  U32 module_info_size;
  U32 sec_con_size;
  U32 sec_map_size;
  U32 file_info_size;
  
  U32 tsm_size;
  U32 mfc_index;
  U32 dbg_header_size;
  U32 ec_info_size;
  
  PDB_DbiHeaderFlags flags;
  COFF_MachineType machine;
  
  U32 reserved;
} PDB_DbiHeader;

//  (this is not "literally" defined by the format - but helpful to have)
typedef enum PDB_DbiRange
{
  PDB_DbiRange_ModuleInfo,
  PDB_DbiRange_SecCon,
  PDB_DbiRange_SecMap,
  PDB_DbiRange_FileInfo,
  PDB_DbiRange_TSM,
  PDB_DbiRange_EcInfo,
  PDB_DbiRange_DbgHeader,
  PDB_DbiRange_COUNT
} PDB_DbiRange;

// "ModuleInfo" DBI range

typedef U32 PDB_DbiSectionContribVersion;
#define PDB_DbiSectionContribVersion_1 (0xeffe0000u + 19970605u)
#define PDB_DbiSectionContribVersion_2 (0xeffe0000u + 20140516u)

typedef struct PDB_DbiSectionContrib40
{
  CV_SectionIndex sec;
  U32 sec_off;
  U32 size;
  U32 flags;
  PDB_ModIndex mod;
} PDB_DbiSectionContrib40;

typedef struct PDB_DbiSectionContrib
{
  PDB_DbiSectionContrib40 base;
  U32 data_crc;
  U32 reloc_crc;
} PDB_DbiSectionContrib;

typedef struct PDB_DbiSectionContrib2
{
  PDB_DbiSectionContrib40 base;
  U32 data_crc;
  U32 reloc_crc;
  U32 sec_coff;
} PDB_DbiSectionContrib2;

typedef struct PDB_DbiCompUnitHeader
{
  U32 unused;
  PDB_DbiSectionContrib contribution;
  U16 flags; // unknown
  
  MSF_StreamNumber sn;
  U32 symbols_size;
  U32 c11_lines_size;
  U32 c13_lines_size;
  
  U16 num_contrib_files;
  U16 unused2;
  U32 file_names_offset;
  
  PDB_StringIndex src_file;
  PDB_StringIndex pdb_file;
  
  // U8[] module_name (null terminated)
  // U8[] obj_name (null terminated)
} PDB_DbiCompUnitHeader;

//  (this is not "literally" defined by the format - but helpful to have)
typedef enum
{
  PDB_DbiCompUnitRange_Symbols,
  PDB_DbiCompUnitRange_C11,
  PDB_DbiCompUnitRange_C13,
  PDB_DbiCompUnitRange_COUNT
} PDB_DbiCompUnitRange;

////////////////////////////////
//~ PDB Format TPI Types

typedef U32 PDB_TpiVersion;
enum
{
  PDB_TpiVersion_INTV_VC2 = 920924,
  PDB_TpiVersion_IMPV40 = 19950410,
  PDB_TpiVersion_IMPV41 = 19951122,
  PDB_TpiVersion_IMPV50_INTERIM = 19960307,
  PDB_TpiVersion_IMPV50 = 19961031,
  PDB_TpiVersion_IMPV70 = 19990903,
  PDB_TpiVersion_IMPV80 = 20040203,
};

typedef struct PDB_TpiHeader
{
  //   (HDR)
  PDB_TpiVersion version;
  U32 header_size;
  U32 ti_lo;
  U32 ti_hi;
  U32 leaf_data_size;
  
  //   (PdbTpiHash)
  MSF_StreamNumber hash_sn;
  MSF_StreamNumber hash_sn_aux;
  U32 hash_key_size;
  U32 hash_bucket_count;
  U32 hash_vals_off;
  U32 hash_vals_size;
  U32 itype_off;
  U32 itype_size;
  U32 hash_adj_off;
  U32 hash_adj_size;
} PDB_TpiHeader;

typedef struct PDB_TpiOffHint
{
  CV_TypeId itype;
  U32 off;
} PDB_TpiOffHint;


////////////////////////////////
//~ PDB Format GSI Types

typedef U32 PDB_GsiSignature;
enum
{
  PDB_GsiSignature_Basic = 0xffffffff,
};

typedef U32 PDB_GsiVersion;
enum
{
  PDB_GsiVersion_V70 = 0xeffe0000 + 19990810,
};

typedef struct PDB_GsiHeader
{
  PDB_GsiSignature signature;
  PDB_GsiVersion version;
  U32 hr_len;
  U32 num_buckets;
} PDB_GsiHeader;

typedef struct PDB_GsiHashRecord
{
  U32 symbol_off;
  U32 cref;
} PDB_GsiHashRecord;

typedef struct PDB_PsiHeader
{
  U32 sym_hash_size;
  U32 addr_map_size;
  U32 thunk_count;
  U32 thunk_size;
  CV_SectionIndex isec_thunk_table;
  U16 padding;
  U32 sec_thunk_table_off;
  U32 sec_count;
} PDB_PsiHeader;

////////////////////////////////
//~ PDB Definition Functions

internal U32 pdb_string_hash1(String8 string);

#endif // PDB_H
