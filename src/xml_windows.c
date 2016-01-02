/*
 * xml_windows.c
 *
 * Set Windows-specific metadata in a WIM file's XML document based on the image
 * contents.
 */

/*
 * Copyright (C) 2016 Eric Biggers
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option) any
 * later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this file; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>

#include "wimlib.h"
#include "wimlib/blob_table.h"
#include "wimlib/dentry.h"
#include "wimlib/endianness.h"
#include "wimlib/error.h"
#include "wimlib/registry.h"
#include "wimlib/wim.h"
#include "wimlib/xml_windows.h"

/* Context for a call to set_windows_specific_info()  */
struct windows_info_ctx {
	WIMStruct *wim;
	int image;
	bool oom_encountered;
	bool debug_enabled;
};

/* For debugging purposes, the environmental variable WIMLIB_DEBUG_XML_INFO can
 * be set to enable messages about certain things not being as expected in the
 * registry or other files used as information sources.  */

#define XML_WARN(format, ...)			\
	if (ctx->debug_enabled)			\
		WARNING(format, ##__VA_ARGS__)

/* Path to the SOFTWARE registry hive  */
static const tchar * const software_hive_path =
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("Windows")
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("System32")
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("config")
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("SOFTWARE");

/* Path to the SYSTEM registry hive  */
static const tchar * const system_hive_path =
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("Windows")
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("System32")
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("config")
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("SYSTEM");

/* Path to kernel32.dll  */
static const tchar * const kernel32_dll_path =
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("Windows")
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("System32")
	WIMLIB_WIM_PATH_SEPARATOR_STRING T("kernel32.dll");

/* Set a property in the XML document, with error checking.  */
static void
set_string_property(struct windows_info_ctx *ctx,
		    const tchar *name, const tchar *value)
{
	int ret = wimlib_set_image_property(ctx->wim, ctx->image, name, value);
	if (likely(!ret))
		return;

	ctx->oom_encountered |= (ret == WIMLIB_ERR_NOMEM);
	WARNING("Failed to set image property \"%"TS"\" to value "
		"\"%"TS"\": %"TS, name, value, wimlib_get_error_string(ret));
}

/* Set a property in the XML document, with error checking.  */
static void
set_number_property(struct windows_info_ctx *ctx, const tchar *name, s64 value)
{
	tchar buffer[32];
	tsprintf(buffer, T("%"PRIi64""), value);
	set_string_property(ctx, name, buffer);
}

/* Check the result of a registry hive operation.  If unsuccessful, possibly
 * print debugging information.  Return true iff successful.  */
static bool
check_hive_status(struct windows_info_ctx *ctx, enum hive_status status,
		  const tchar *key, const tchar *value)
{
	if (likely(status == HIVE_OK))
		return true;

	ctx->oom_encountered |= (status == HIVE_OUT_OF_MEMORY);
	XML_WARN("%s; key=%"TS" value=%"TS, hive_status_to_string(status),
		 (key ? key : T("(null)")), (value ? value : T("(null)")));
	return false;
}

static bool
is_registry_valid(struct windows_info_ctx *ctx, const void *hive_mem,
		  size_t hive_size)
{
	enum hive_status status;

	status = hive_validate(hive_mem, hive_size);
	return check_hive_status(ctx, status, NULL, NULL);
}

static bool
get_string_from_registry(struct windows_info_ctx *ctx, const struct regf *regf,
			 const tchar *key_name, const tchar *value_name,
			 tchar **value_ret)
{
	enum hive_status status;

	status = hive_get_string(regf, key_name, value_name, value_ret);
	return check_hive_status(ctx, status, key_name, value_name);
}

static bool
get_number_from_registry(struct windows_info_ctx *ctx, const struct regf *regf,
			 const tchar *key_name, const tchar *value_name,
			 s64 *value_ret)
{
	enum hive_status status;

	status = hive_get_number(regf, key_name, value_name, value_ret);
	return check_hive_status(ctx, status, key_name, value_name);
}

static bool
list_subkeys_in_registry(struct windows_info_ctx *ctx, const struct regf *regf,
			 const tchar *key_name, tchar ***subkeys_ret)
{
	enum hive_status status;

	status = hive_list_subkeys(regf, key_name, subkeys_ret);
	return check_hive_status(ctx, status, key_name, NULL);
}

/* Copy a string value from a registry hive to the XML document.  */
static void
copy_registry_string(struct windows_info_ctx *ctx, const struct regf *regf,
		     const tchar *key_name, const tchar *value_name,
		     const tchar *property_name)
{
	tchar *string;

	if (get_string_from_registry(ctx, regf, key_name, value_name, &string)) {
		set_string_property(ctx, property_name, string);
		FREE(string);
	}
}

/* A table that map Windows language IDs, sorted numerically, to their language
 * names.  It was generated by tools/generate_language_id_map.c.  */
static const struct {
	u16 id;
	u16 name_start_offset;
} language_id_map[452] = {
	{0x0000,    0}, {0x0001,    6}, {0x0002,   12}, {0x0003,   18},
	{0x0004,   24}, {0x0005,   30}, {0x0006,   36}, {0x0007,   42},
	{0x0008,   48}, {0x0009,   54}, {0x000a,   60}, {0x000b,   66},
	{0x000c,   72}, {0x000d,   78}, {0x000e,   84}, {0x000f,   90},
	{0x0010,   96}, {0x0011,  102}, {0x0012,  108}, {0x0013,  114},
	{0x0014,  120}, {0x0015,  126}, {0x0016,  132}, {0x0017,  138},
	{0x0018,  144}, {0x0019,  150}, {0x001a,  156}, {0x001b,  162},
	{0x001c,  168}, {0x001d,  174}, {0x001e,  180}, {0x001f,  186},
	{0x0020,  192}, {0x0021,  198}, {0x0022,  204}, {0x0023,  210},
	{0x0024,  216}, {0x0025,  222}, {0x0026,  228}, {0x0027,  234},
	{0x0028,  240}, {0x0029,  251}, {0x002a,  257}, {0x002b,  263},
	{0x002c,  269}, {0x002d,  280}, {0x002e,  286}, {0x002f,  293},
	{0x0030,  299}, {0x0031,  305}, {0x0032,  311}, {0x0033,  317},
	{0x0034,  323}, {0x0035,  329}, {0x0036,  335}, {0x0037,  341},
	{0x0038,  347}, {0x0039,  353}, {0x003a,  359}, {0x003b,  365},
	{0x003c,  371}, {0x003d,  377}, {0x003e,  384}, {0x003f,  390},
	{0x0040,  396}, {0x0041,  402}, {0x0042,  408}, {0x0043,  414},
	{0x0044,  425}, {0x0045,  431}, {0x0046,  437}, {0x0047,  443},
	{0x0048,  449}, {0x0049,  455}, {0x004a,  461}, {0x004b,  467},
	{0x004c,  473}, {0x004d,  479}, {0x004e,  485}, {0x004f,  491},
	{0x0050,  497}, {0x0051,  503}, {0x0052,  509}, {0x0053,  515},
	{0x0054,  521}, {0x0055,  527}, {0x0056,  533}, {0x0057,  539},
	{0x0058,  546}, {0x0059,  553}, {0x005a,  564}, {0x005b,  571},
	{0x005c,  577}, {0x005d,  589}, {0x005e,  600}, {0x005f,  606},
	{0x0060,  618}, {0x0061,  629}, {0x0062,  635}, {0x0063,  641},
	{0x0064,  647}, {0x0065,  654}, {0x0066,  660}, {0x0067,  667},
	{0x0068,  678}, {0x0069,  689}, {0x006a,  696}, {0x006b,  702},
	{0x006c,  709}, {0x006d,  716}, {0x006e,  722}, {0x006f,  728},
	{0x0070,  734}, {0x0071,  740}, {0x0072,  746}, {0x0073,  752},
	{0x0074,  758}, {0x0075,  764}, {0x0076,  771}, {0x0077,  778},
	{0x0078,  784}, {0x0079,  790}, {0x007a,  798}, {0x007c,  805},
	{0x007e,  812}, {0x007f,  818}, {0x0080,  819}, {0x0081,  825},
	{0x0082,  831}, {0x0083,  837}, {0x0084,  843}, {0x0085,  850},
	{0x0086,  857}, {0x0087,  869}, {0x0088,  875}, {0x008c,  881},
	{0x0091,  888}, {0x0092,  894}, {0x0400,  905}, {0x0401,  911},
	{0x0402,  917}, {0x0403,  923}, {0x0404,  929}, {0x0405,  935},
	{0x0406,  941}, {0x0407,  947}, {0x0408,  953}, {0x0409,  959},
	{0x040a,  965}, {0x040b,  978}, {0x040c,  984}, {0x040d,  990},
	{0x040e,  996}, {0x040f, 1002}, {0x0410, 1008}, {0x0411, 1014},
	{0x0412, 1020}, {0x0413, 1026}, {0x0414, 1032}, {0x0415, 1038},
	{0x0416, 1044}, {0x0417, 1050}, {0x0418, 1056}, {0x0419, 1062},
	{0x041a, 1068}, {0x041b, 1074}, {0x041c, 1080}, {0x041d, 1086},
	{0x041e, 1092}, {0x041f, 1098}, {0x0420, 1104}, {0x0421, 1110},
	{0x0422, 1116}, {0x0423, 1122}, {0x0424, 1128}, {0x0425, 1134},
	{0x0426, 1140}, {0x0427, 1146}, {0x0428, 1152}, {0x0429, 1163},
	{0x042a, 1169}, {0x042b, 1175}, {0x042c, 1181}, {0x042d, 1192},
	{0x042e, 1198}, {0x042f, 1205}, {0x0430, 1211}, {0x0431, 1217},
	{0x0432, 1223}, {0x0433, 1229}, {0x0434, 1235}, {0x0435, 1241},
	{0x0436, 1247}, {0x0437, 1253}, {0x0438, 1259}, {0x0439, 1265},
	{0x043a, 1271}, {0x043b, 1277}, {0x043d, 1283}, {0x043e, 1290},
	{0x043f, 1296}, {0x0440, 1302}, {0x0441, 1308}, {0x0442, 1314},
	{0x0443, 1320}, {0x0444, 1331}, {0x0445, 1337}, {0x0446, 1343},
	{0x0447, 1349}, {0x0448, 1355}, {0x0449, 1361}, {0x044a, 1367},
	{0x044b, 1373}, {0x044c, 1379}, {0x044d, 1385}, {0x044e, 1391},
	{0x044f, 1397}, {0x0450, 1403}, {0x0451, 1409}, {0x0452, 1415},
	{0x0453, 1421}, {0x0454, 1427}, {0x0455, 1433}, {0x0456, 1439},
	{0x0457, 1445}, {0x0458, 1452}, {0x0459, 1459}, {0x045a, 1470},
	{0x045b, 1477}, {0x045c, 1483}, {0x045d, 1495}, {0x045e, 1506},
	{0x045f, 1512}, {0x0460, 1524}, {0x0461, 1535}, {0x0462, 1541},
	{0x0463, 1547}, {0x0464, 1553}, {0x0465, 1560}, {0x0466, 1566},
	{0x0467, 1573}, {0x0468, 1579}, {0x0469, 1590}, {0x046a, 1597},
	{0x046b, 1603}, {0x046c, 1610}, {0x046d, 1617}, {0x046e, 1623},
	{0x046f, 1629}, {0x0470, 1635}, {0x0471, 1641}, {0x0472, 1647},
	{0x0473, 1653}, {0x0474, 1659}, {0x0475, 1665}, {0x0476, 1672},
	{0x0477, 1679}, {0x0478, 1685}, {0x0479, 1691}, {0x047a, 1699},
	{0x047c, 1706}, {0x047e, 1713}, {0x0480, 1719}, {0x0481, 1725},
	{0x0482, 1731}, {0x0483, 1737}, {0x0484, 1743}, {0x0485, 1750},
	{0x0486, 1757}, {0x0487, 1769}, {0x0488, 1775}, {0x048c, 1781},
	{0x0491, 1788}, {0x0492, 1794}, {0x0501, 1805}, {0x05fe, 1814},
	{0x0800, 1824}, {0x0801, 1830}, {0x0803, 1836}, {0x0804, 1851},
	{0x0807, 1857}, {0x0809, 1863}, {0x080a, 1869}, {0x080c, 1875},
	{0x0810, 1881}, {0x0813, 1887}, {0x0814, 1893}, {0x0816, 1899},
	{0x0818, 1905}, {0x0819, 1911}, {0x081a, 1917}, {0x081d, 1928},
	{0x0820, 1934}, {0x082c, 1940}, {0x082e, 1951}, {0x0832, 1958},
	{0x083b, 1964}, {0x083c, 1970}, {0x083e, 1976}, {0x0843, 1982},
	{0x0845, 1993}, {0x0846, 1999}, {0x0849, 2010}, {0x0850, 2016},
	{0x0859, 2027}, {0x085d, 2038}, {0x085f, 2049}, {0x0860, 2061},
	{0x0861, 2072}, {0x0867, 2078}, {0x086b, 2089}, {0x0873, 2096},
	{0x0901, 2102}, {0x09ff, 2116}, {0x0c00, 2126}, {0x0c01, 2132},
	{0x0c04, 2138}, {0x0c07, 2144}, {0x0c09, 2150}, {0x0c0a, 2156},
	{0x0c0c, 2162}, {0x0c1a, 2168}, {0x0c3b, 2179}, {0x0c50, 2185},
	{0x0c51, 2196}, {0x0c6b, 2202}, {0x1000, 2209}, {0x1001, 2220},
	{0x1004, 2226}, {0x1007, 2232}, {0x1009, 2238}, {0x100a, 2244},
	{0x100c, 2250}, {0x101a, 2256}, {0x103b, 2262}, {0x105f, 2269},
	{0x1401, 2281}, {0x1404, 2287}, {0x1407, 2293}, {0x1409, 2299},
	{0x140a, 2305}, {0x140c, 2311}, {0x141a, 2317}, {0x143b, 2328},
	{0x1801, 2335}, {0x1809, 2341}, {0x180a, 2347}, {0x180c, 2353},
	{0x181a, 2359}, {0x183b, 2370}, {0x1c01, 2377}, {0x1c09, 2383},
	{0x1c0a, 2389}, {0x1c0c, 2395}, {0x1c1a, 2402}, {0x1c3b, 2413},
	{0x2000, 2420}, {0x2001, 2426}, {0x2009, 2432}, {0x200a, 2438},
	{0x200c, 2444}, {0x201a, 2450}, {0x203b, 2461}, {0x2400, 2468},
	{0x2401, 2474}, {0x2409, 2480}, {0x240a, 2487}, {0x240c, 2493},
	{0x241a, 2499}, {0x243b, 2510}, {0x2800, 2517}, {0x2801, 2523},
	{0x2809, 2529}, {0x280a, 2535}, {0x280c, 2541}, {0x281a, 2547},
	{0x2c00, 2558}, {0x2c01, 2564}, {0x2c09, 2570}, {0x2c0a, 2576},
	{0x2c0c, 2582}, {0x2c1a, 2588}, {0x3000, 2599}, {0x3001, 2605},
	{0x3009, 2611}, {0x300a, 2617}, {0x300c, 2623}, {0x301a, 2629},
	{0x3400, 2640}, {0x3401, 2646}, {0x3409, 2652}, {0x340a, 2658},
	{0x340c, 2664}, {0x3800, 2670}, {0x3801, 2676}, {0x3809, 2682},
	{0x380a, 2688}, {0x380c, 2694}, {0x3c00, 2700}, {0x3c01, 2706},
	{0x3c09, 2712}, {0x3c0a, 2718}, {0x3c0c, 2724}, {0x4000, 2730},
	{0x4001, 2736}, {0x4009, 2742}, {0x400a, 2748}, {0x4400, 2754},
	{0x4409, 2760}, {0x440a, 2766}, {0x4800, 2772}, {0x4809, 2778},
	{0x480a, 2784}, {0x4c00, 2790}, {0x4c0a, 2796}, {0x500a, 2802},
	{0x540a, 2808}, {0x580a, 2814}, {0x5c0a, 2821}, {0x641a, 2827},
	{0x681a, 2838}, {0x6c1a, 2849}, {0x701a, 2860}, {0x703b, 2871},
	{0x742c, 2878}, {0x743b, 2889}, {0x7804, 2896}, {0x7814, 2902},
	{0x781a, 2908}, {0x782c, 2919}, {0x783b, 2930}, {0x7843, 2937},
	{0x7850, 2948}, {0x785d, 2954}, {0x785f, 2965}, {0x7c04, 2977},
	{0x7c14, 2983}, {0x7c1a, 2989}, {0x7c28, 3000}, {0x7c2e, 3011},
	{0x7c3b, 3018}, {0x7c43, 3025}, {0x7c46, 3036}, {0x7c50, 3047},
	{0x7c59, 3058}, {0x7c5c, 3069}, {0x7c5d, 3081}, {0x7c5f, 3092},
	{0x7c67, 3104}, {0x7c68, 3115}, {0x7c86, 3126}, {0x7c92, 3138},
};

/* All the language names; generated by tools/generate_language_id_map.c.
 * For compactness, this is a 'char' string rather than a 'tchar' string.  */
static const char language_names[3149] =
	"en-US\0ar-SA\0bg-BG\0ca-ES\0zh-CN\0cs-CZ\0da-DK\0de-DE\0el-GR\0en-US\0"
	"es-ES\0fi-FI\0fr-FR\0he-IL\0hu-HU\0is-IS\0it-IT\0ja-JP\0ko-KR\0nl-NL\0"
	"nb-NO\0pl-PL\0pt-BR\0rm-CH\0ro-RO\0ru-RU\0hr-HR\0sk-SK\0sq-AL\0sv-SE\0"
	"th-TH\0tr-TR\0ur-PK\0id-ID\0uk-UA\0be-BY\0sl-SI\0et-EE\0lv-LV\0lt-LT\0"
	"tg-Cyrl-TJ\0fa-IR\0vi-VN\0hy-AM\0az-Latn-AZ\0eu-ES\0hsb-DE\0mk-MK\0"
	"st-ZA\0ts-ZA\0tn-ZA\0ve-ZA\0xh-ZA\0zu-ZA\0af-ZA\0ka-GE\0fo-FO\0hi-IN\0"
	"mt-MT\0se-NO\0ga-IE\0yi-001\0ms-MY\0kk-KZ\0ky-KG\0sw-KE\0tk-TM\0"
	"uz-Latn-UZ\0tt-RU\0bn-IN\0pa-IN\0gu-IN\0or-IN\0ta-IN\0te-IN\0kn-IN\0"
	"ml-IN\0as-IN\0mr-IN\0sa-IN\0mn-MN\0bo-CN\0cy-GB\0km-KH\0lo-LA\0my-MM\0"
	"gl-ES\0kok-IN\0mni-IN\0sd-Arab-PK\0syr-SY\0si-LK\0chr-Cher-US\0"
	"iu-Latn-CA\0am-ET\0tzm-Latn-DZ\0ks-Arab-IN\0ne-NP\0fy-NL\0ps-AF\0"
	"fil-PH\0dv-MV\0bin-NG\0ff-Latn-SN\0ha-Latn-NG\0ibb-NG\0yo-NG\0quz-BO\0"
	"nso-ZA\0ba-RU\0lb-LU\0kl-GL\0ig-NG\0kr-NG\0om-ET\0ti-ER\0gn-PY\0"
	"haw-US\0la-001\0so-SO\0ii-CN\0pap-029\0arn-CL\0moh-CA\0br-FR\0\0"
	"ug-CN\0mi-NZ\0oc-FR\0co-FR\0gsw-FR\0sah-RU\0quc-Latn-GT\0rw-RW\0"
	"wo-SN\0prs-AF\0gd-GB\0ku-Arab-IQ\0en-US\0ar-SA\0bg-BG\0ca-ES\0zh-TW\0"
	"cs-CZ\0da-DK\0de-DE\0el-GR\0en-US\0es-ES_tradnl\0fi-FI\0fr-FR\0he-IL\0"
	"hu-HU\0is-IS\0it-IT\0ja-JP\0ko-KR\0nl-NL\0nb-NO\0pl-PL\0pt-BR\0rm-CH\0"
	"ro-RO\0ru-RU\0hr-HR\0sk-SK\0sq-AL\0sv-SE\0th-TH\0tr-TR\0ur-PK\0id-ID\0"
	"uk-UA\0be-BY\0sl-SI\0et-EE\0lv-LV\0lt-LT\0tg-Cyrl-TJ\0fa-IR\0vi-VN\0"
	"hy-AM\0az-Latn-AZ\0eu-ES\0hsb-DE\0mk-MK\0st-ZA\0ts-ZA\0tn-ZA\0ve-ZA\0"
	"xh-ZA\0zu-ZA\0af-ZA\0ka-GE\0fo-FO\0hi-IN\0mt-MT\0se-NO\0yi-001\0"
	"ms-MY\0kk-KZ\0ky-KG\0sw-KE\0tk-TM\0uz-Latn-UZ\0tt-RU\0bn-IN\0pa-IN\0"
	"gu-IN\0or-IN\0ta-IN\0te-IN\0kn-IN\0ml-IN\0as-IN\0mr-IN\0sa-IN\0mn-MN\0"
	"bo-CN\0cy-GB\0km-KH\0lo-LA\0my-MM\0gl-ES\0kok-IN\0mni-IN\0sd-Deva-IN\0"
	"syr-SY\0si-LK\0chr-Cher-US\0iu-Cans-CA\0am-ET\0tzm-Arab-MA\0"
	"ks-Arab-IN\0ne-NP\0fy-NL\0ps-AF\0fil-PH\0dv-MV\0bin-NG\0ff-NG\0"
	"ha-Latn-NG\0ibb-NG\0yo-NG\0quz-BO\0nso-ZA\0ba-RU\0lb-LU\0kl-GL\0"
	"ig-NG\0kr-NG\0om-ET\0ti-ET\0gn-PY\0haw-US\0la-001\0so-SO\0ii-CN\0"
	"pap-029\0arn-CL\0moh-CA\0br-FR\0ug-CN\0mi-NZ\0oc-FR\0co-FR\0gsw-FR\0"
	"sah-RU\0quc-Latn-GT\0rw-RW\0wo-SN\0prs-AF\0gd-GB\0ku-Arab-IQ\0"
	"qps-ploc\0qps-ploca\0en-US\0ar-IQ\0ca-ES-valencia\0zh-CN\0de-CH\0"
	"en-GB\0es-MX\0fr-BE\0it-CH\0nl-BE\0nn-NO\0pt-PT\0ro-MD\0ru-MD\0"
	"sr-Latn-CS\0sv-FI\0ur-IN\0az-Cyrl-AZ\0dsb-DE\0tn-BW\0se-SE\0ga-IE\0"
	"ms-BN\0uz-Cyrl-UZ\0bn-BD\0pa-Arab-PK\0ta-LK\0mn-Mong-CN\0sd-Arab-PK\0"
	"iu-Latn-CA\0tzm-Latn-DZ\0ks-Deva-IN\0ne-IN\0ff-Latn-SN\0quz-EC\0"
	"ti-ER\0qps-Latn-x-sh\0qps-plocm\0en-US\0ar-EG\0zh-HK\0de-AT\0en-AU\0"
	"es-ES\0fr-CA\0sr-Cyrl-CS\0se-FI\0mn-Mong-MN\0dz-BT\0quz-PE\0"
	"ks-Arab-IN\0ar-LY\0zh-SG\0de-LU\0en-CA\0es-GT\0fr-CH\0hr-BA\0smj-NO\0"
	"tzm-Tfng-MA\0ar-DZ\0zh-MO\0de-LI\0en-NZ\0es-CR\0fr-LU\0bs-Latn-BA\0"
	"smj-SE\0ar-MA\0en-IE\0es-PA\0fr-MC\0sr-Latn-BA\0sma-NO\0ar-TN\0en-ZA\0"
	"es-DO\0fr-029\0sr-Cyrl-BA\0sma-SE\0en-US\0ar-OM\0en-JM\0es-VE\0fr-RE\0"
	"bs-Cyrl-BA\0sms-FI\0en-US\0ar-YE\0en-029\0es-CO\0fr-CD\0sr-Latn-RS\0"
	"smn-FI\0en-US\0ar-SY\0en-BZ\0es-PE\0fr-SN\0sr-Cyrl-RS\0en-US\0ar-JO\0"
	"en-TT\0es-AR\0fr-CM\0sr-Latn-ME\0en-US\0ar-LB\0en-ZW\0es-EC\0fr-CI\0"
	"sr-Cyrl-ME\0en-US\0ar-KW\0en-PH\0es-CL\0fr-ML\0en-US\0ar-AE\0en-ID\0"
	"es-UY\0fr-MA\0en-US\0ar-BH\0en-HK\0es-PY\0fr-HT\0en-US\0ar-QA\0en-IN\0"
	"es-BO\0en-US\0en-MY\0es-SV\0en-US\0en-SG\0es-HN\0en-US\0es-NI\0es-PR\0"
	"es-US\0es-419\0es-CU\0bs-Cyrl-BA\0bs-Latn-BA\0sr-Cyrl-RS\0sr-Latn-RS\0"
	"smn-FI\0az-Cyrl-AZ\0sms-FI\0zh-CN\0nn-NO\0bs-Latn-BA\0az-Latn-AZ\0"
	"sma-SE\0uz-Cyrl-UZ\0mn-MN\0iu-Cans-CA\0tzm-Tfng-MA\0zh-HK\0nb-NO\0"
	"sr-Latn-RS\0tg-Cyrl-TJ\0dsb-DE\0smj-SE\0uz-Latn-UZ\0pa-Arab-PK\0"
	"mn-Mong-CN\0sd-Arab-PK\0chr-Cher-US\0iu-Latn-CA\0tzm-Latn-DZ\0"
	"ff-Latn-SN\0ha-Latn-NG\0quc-Latn-GT\0ku-Arab-IQ\0";

/* Translate a Windows language ID to its name.  Returns NULL if the ID is not
 * recognized.  */
static const char *
language_id_to_name(u16 id)
{
	int l = 0;
	int r = ARRAY_LEN(language_id_map) - 1;
	do {
		int m = (l + r) / 2;
		if (id < language_id_map[m].id)
			r = m - 1;
		else if (id > language_id_map[m].id)
			l = m + 1;
		else
			return &language_names[language_id_map[m].name_start_offset];
	} while (l <= r);
	return NULL;
}

/* PE binary processor architecture codes (common ones only)  */
#define IMAGE_FILE_MACHINE_I386		0x014C
#define IMAGE_FILE_MACHINE_ARM		0x01C0
#define IMAGE_FILE_MACHINE_ARMV7	0x01C4
#define IMAGE_FILE_MACHINE_THUMB	0x01C2
#define IMAGE_FILE_MACHINE_IA64		0x0200
#define IMAGE_FILE_MACHINE_AMD64	0x8664
#define IMAGE_FILE_MACHINE_ARM64	0xAA64

/* Windows API processor architecture codes (common ones only)  */
#define PROCESSOR_ARCHITECTURE_INTEL	0
#define PROCESSOR_ARCHITECTURE_ARM	5
#define PROCESSOR_ARCHITECTURE_IA64	6
#define PROCESSOR_ARCHITECTURE_AMD64	9
#define PROCESSOR_ARCHITECTURE_ARM64	12

/* Translate a processor architecture code as given in a PE binary to the code
 * used by the Windows API.  Returns -1 if the code is not recognized.  */
static int
pe_arch_to_windows_arch(unsigned pe_arch)
{
	switch (pe_arch) {
	case IMAGE_FILE_MACHINE_I386:
		return PROCESSOR_ARCHITECTURE_INTEL;
	case IMAGE_FILE_MACHINE_ARM:
	case IMAGE_FILE_MACHINE_ARMV7:
	case IMAGE_FILE_MACHINE_THUMB:
		return PROCESSOR_ARCHITECTURE_ARM;
	case IMAGE_FILE_MACHINE_IA64:
		return PROCESSOR_ARCHITECTURE_IA64;
	case IMAGE_FILE_MACHINE_AMD64:
		return PROCESSOR_ARCHITECTURE_AMD64;
	case IMAGE_FILE_MACHINE_ARM64:
		return PROCESSOR_ARCHITECTURE_ARM64;
	}
	return -1;
}

/* Gather information from kernel32.dll.  */
static void
set_info_from_kernel32(struct windows_info_ctx *ctx,
		       const void *contents, size_t size)
{
	u32 e_lfanew;
	const u8 *pe_hdr;
	unsigned pe_arch;
	int arch;

	/* Read the processor architecture from the executable header.  */

	if (size < 0x40)
		goto invalid;

	e_lfanew = le32_to_cpu(*(le32 *)((u8 *)contents + 0x3C));
	if (e_lfanew > size || size - e_lfanew < 6 || (e_lfanew & 3))
		goto invalid;

	pe_hdr = (u8 *)contents + e_lfanew;
	if (*(u32 *)pe_hdr != cpu_to_le32(0x00004550))	/* "PE\0\0"  */
		goto invalid;

	pe_arch = le16_to_cpu(*(le16 *)(pe_hdr + 4));
	arch = pe_arch_to_windows_arch(pe_arch);
	if (arch >= 0) {
		/* Save the processor architecture in the XML document.  */
		set_number_property(ctx, T("WINDOWS/ARCH"), arch);
	} else {
		XML_WARN("Architecture value %x from kernel32.dll "
			 "header not recognized", pe_arch);
	}
	return;

invalid:
	XML_WARN("kernel32.dll is not a valid PE binary.");
}

/* Gather information from the SOFTWARE registry hive.  */
static void
set_info_from_software_hive(struct windows_info_ctx *ctx,
			    const struct regf *regf)
{
	const tchar *version_key = T("Microsoft\\Windows NT\\CurrentVersion");
	s64 major_version = -1;
	s64 minor_version = -1;
	tchar *version_string;

	/* Image flags  */
	copy_registry_string(ctx, regf, version_key, T("EditionID"),
			     T("FLAGS"));

	/* Image display name  */
	copy_registry_string(ctx, regf, version_key, T("ProductName"),
			     T("DISPLAYNAME"));

	/* Image display description  */
	copy_registry_string(ctx, regf, version_key, T("ProductName"),
			     T("DISPLAYDESCRIPTION"));

	/* Edition ID  */
	copy_registry_string(ctx, regf, version_key, T("EditionID"),
			     T("WINDOWS/EDITIONID"));

	/* Installation type  */
	copy_registry_string(ctx, regf, version_key, T("InstallationType"),
			     T("WINDOWS/INSTALLATIONTYPE"));

	/* Product name  */
	copy_registry_string(ctx, regf, version_key, T("ProductName"),
			     T("WINDOWS/PRODUCTNAME"));

	/* Major and minor version number  */

	/* Note: in Windows 10, CurrentVersion was apparently fixed at 6.3.
	 * Instead, the new values CurrentMajorVersionNumber and
	 * CurrentMinorVersionNumber should be used.  */

	get_number_from_registry(ctx, regf, version_key,
				 T("CurrentMajorVersionNumber"), &major_version);

	get_number_from_registry(ctx, regf, version_key,
				 T("CurrentMinorVersionNumber"), &minor_version);

	if (major_version < 0 || minor_version < 0) {
		if (get_string_from_registry(ctx, regf, version_key,
					     T("CurrentVersion"),
					     &version_string))
		{
			if (2 != tscanf(version_string, T("%"PRIi64".%"PRIi64),
					&major_version, &minor_version))
			{
				XML_WARN("Unrecognized CurrentVersion: %"TS,
					 version_string);
			}
			FREE(version_string);
		}
	}

	if (major_version >= 0) {
		set_number_property(ctx, T("WINDOWS/VERSION/MAJOR"),
				    major_version);
		if (minor_version >= 0) {
			set_number_property(ctx, T("WINDOWS/VERSION/MINOR"),
					    minor_version);
		}
	}

	/* Build number  */
	copy_registry_string(ctx, regf, version_key, T("CurrentBuild"),
			     T("WINDOWS/VERSION/BUILD"));
}

/* Gather the default language from the SYSTEM registry hive.  */
static void
set_default_language(struct windows_info_ctx *ctx, const struct regf *regf)
{
	tchar *string;
	unsigned language_id;

	if (!get_string_from_registry(ctx, regf,
				      T("ControlSet001\\Control\\Nls\\Language"),
				      T("InstallLanguage"), &string))
		return;

	if (1 == tscanf(string, T("%x"), &language_id)) {
		const char *language_name = language_id_to_name(language_id);
		if (language_name) {
			size_t len = strlen(language_name);
			tchar tstr[len + 1];
			for (size_t i = 0; i <= len; i++)
				tstr[i] = language_name[i];
			set_string_property(ctx, T("WINDOWS/LANGUAGES/DEFAULT"),
					    tstr);
			FREE(string);
			return;
		}
	}
	XML_WARN("Unrecognized InstallLanguage: %"TS, string);
	FREE(string);
}

/* Gather information from the SYSTEM registry hive.  */
static void
set_info_from_system_hive(struct windows_info_ctx *ctx, const struct regf *regf)
{
	const tchar *windows_key = T("ControlSet001\\Control\\Windows");
	const tchar *uilanguages_key = T("ControlSet001\\Control\\MUI\\UILanguages");
	const tchar *productoptions_key = T("ControlSet001\\Control\\ProductOptions");
	s64 spbuild;
	s64 splevel;
	tchar **subkeys;

	/* Service pack build  */
	if (get_number_from_registry(ctx, regf, windows_key,
				     T("CSDBuildNumber"), &spbuild))
		set_number_property(ctx, T("WINDOWS/VERSION/SPBUILD"), spbuild);

	/* Service pack level  */
	if (get_number_from_registry(ctx, regf, windows_key,
				     T("CSDVersion"), &splevel))
		set_number_property(ctx, T("WINDOWS/VERSION/SPLEVEL"), splevel >> 8);

	/* Product type  */
	copy_registry_string(ctx, regf, productoptions_key, T("ProductType"),
			     T("WINDOWS/PRODUCTTYPE"));

	/* Product suite  */
	copy_registry_string(ctx, regf, productoptions_key, T("ProductSuite"),
			     T("WINDOWS/PRODUCTSUITE"));

	/* Hardware abstraction layer  */
	copy_registry_string(ctx, regf,
			     T("ControlSet001\\Control\\Class\\{4D36E966-E325-11CE-BFC1-08002BE10318}\\0000"),
			     T("MatchingDeviceId"),
			     T("WINDOWS/HAL"));

	/* Languages  */
	if (list_subkeys_in_registry(ctx, regf, uilanguages_key, &subkeys)) {
		tchar property_name[64];
		for (tchar **p = subkeys; *p; p++) {
			tsprintf(property_name,
				 T("WINDOWS/LANGUAGES/LANGUAGE[%zu]"), p - subkeys + 1);
			set_string_property(ctx, property_name, *p);
		}
		hive_free_subkeys_list(subkeys);
	}

	/* Default language  */
	set_default_language(ctx, regf);
}

/* Load the contents of a file in the currently selected WIM image into memory.
 */
static void *
load_file_contents(struct windows_info_ctx *ctx,
		   const tchar *path, size_t *size_ret)
{
	const struct wim_dentry *dentry;
	const struct blob_descriptor *blob;
	void *contents;
	int ret;

	dentry = get_dentry(ctx->wim, path, WIMLIB_CASE_INSENSITIVE);
	if (!dentry) {
		XML_WARN("File \"%"TS"\" not found", path);
		return NULL;
	}

	blob = inode_get_blob_for_unnamed_data_stream(dentry->d_inode,
						      ctx->wim->blob_table);
	if (!blob) {
		XML_WARN("File \"%"TS"\" has no contents", path);
		return NULL;
	}

	ret = read_blob_into_alloc_buf(blob, &contents);
	if (ret) {
		XML_WARN("Error loading file \"%"TS"\" (size=%"PRIu64"): %"TS,
			 path, blob->size, wimlib_get_error_string(ret));
		ctx->oom_encountered |= (ret == WIMLIB_ERR_NOMEM &&
					 blob->size < 100000000);
		return NULL;
	}

	*size_ret = blob->size;
	return contents;
}

/* Load and validate a registry hive file.  */
static void *
load_hive(struct windows_info_ctx *ctx, const tchar *path)
{
	void *hive_mem;
	size_t hive_size;

	hive_mem = load_file_contents(ctx, path, &hive_size);
	if (hive_mem && !is_registry_valid(ctx, hive_mem, hive_size)) {
		XML_WARN("\"%"TS"\" is not a valid registry hive!", path);
		FREE(hive_mem);
		hive_mem = NULL;
	}
	return hive_mem;
}

/*
 * Set Windows-specific XML information for the currently selected WIM image.
 *
 * This process is heavily based on heuristics and hard-coded logic related to
 * where Windows stores certain types of information.  Therefore, it simply
 * tries to set as much information as possible.  If there's a problem, it skips
 * the affected information and proceeds to the next part.  It only returns an
 * error code if there was a severe problem such as out-of-memory.
 */
int
set_windows_specific_info(WIMStruct *wim)
{
	void *contents;
	size_t size;
	struct windows_info_ctx _ctx = {
		.wim = wim,
		.image = wim->current_image,
		.oom_encountered = false,
		.debug_enabled = (tgetenv(T("WIMLIB_DEBUG_XML_INFO")) != NULL),
	}, *ctx = &_ctx;

	if ((contents = load_file_contents(ctx, kernel32_dll_path, &size))) {
		set_string_property(ctx, T("WINDOWS/SYSTEMROOT"), T("WINDOWS"));
		set_info_from_kernel32(ctx, contents, size);
		FREE(contents);
	}

	if ((contents = load_hive(ctx, software_hive_path))) {
		set_info_from_software_hive(ctx, contents);
		FREE(contents);
	}

	if ((contents = load_hive(ctx, system_hive_path))) {
		set_info_from_system_hive(ctx, contents);
		FREE(contents);
	}

	if (ctx->oom_encountered) {
		ERROR("Ran out of memory while setting Windows-specific "
		      "metadata in the WIM file's XML document.");
		return WIMLIB_ERR_NOMEM;
	}

	return 0;
}
