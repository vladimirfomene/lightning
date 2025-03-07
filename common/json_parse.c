/* JSON core and helpers */
#include "config.h"
#include <assert.h>
#include <bitcoin/preimage.h>
#include <bitcoin/privkey.h>
#include <bitcoin/psbt.h>
#include <bitcoin/pubkey.h>
#include <bitcoin/short_channel_id.h>
#include <bitcoin/tx.h>
#include <ccan/json_escape/json_escape.h>
#include <ccan/mem/mem.h>
#include <ccan/str/hex/hex.h>
#include <ccan/tal/str/str.h>
#include <ccan/time/time.h>
#include <common/amount.h>
#include <common/channel_id.h>
#include <common/json_parse.h>
#include <common/node_id.h>
#include <common/overflows.h>
#include <common/utils.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <wire/onion_wire.h>

bool json_to_s64(const char *buffer, const jsmntok_t *tok, s64 *num)
{
	char *end;
	long long l;

	l = strtoll(buffer + tok->start, &end, 0);
	if (end != buffer + tok->end)
		return false;

	BUILD_ASSERT(sizeof(l) >= sizeof(*num));
	*num = l;

	/* Check for overflow/underflow */
	if ((l == LONG_MAX || l == LONG_MIN) && errno == ERANGE)
		return false;

	/* Check if the number did not fit in `s64` (in case `long long`
	is a bigger type). */
	if (*num != l)
		return false;

	return true;
}

bool json_to_millionths(const char *buffer, const jsmntok_t *tok,
			u64 *millionths)
{
	int decimal_places = -1;
	bool has_digits = 0;

	*millionths = 0;
	for (int i = tok->start; i < tok->end; i++) {
		if (isdigit(buffer[i])) {
			has_digits = true;
			/* Ignore too much precision */
			if (decimal_places >= 0 && ++decimal_places > 6)
				continue;
			if (mul_overflows_u64(*millionths, 10))
				return false;
			*millionths *= 10;
			if (add_overflows_u64(*millionths, buffer[i] - '0'))
				return false;
			*millionths += buffer[i] - '0';
		} else if (buffer[i] == '.') {
			if (decimal_places != -1)
				return false;
			decimal_places = 0;
		} else
			return false;
	}

	if (!has_digits)
		return false;

	if (decimal_places == -1)
		decimal_places = 0;

	while (decimal_places < 6) {
		if (mul_overflows_u64(*millionths, 10))
			return false;
		*millionths *= 10;
		decimal_places++;
	}
	return true;
}

bool json_to_number(const char *buffer, const jsmntok_t *tok,
		    unsigned int *num)
{
	uint64_t u64;

	if (!json_to_u64(buffer, tok, &u64))
		return false;
	*num = u64;

	/* Just in case it doesn't fit. */
	if (*num != u64)
		return false;
	return true;
}

bool json_to_u16(const char *buffer, const jsmntok_t *tok,
		 short unsigned int *num)
{
	uint64_t u64;

	if (!json_to_u64(buffer, tok, &u64))
		return false;
	*num = u64;

	/* Just in case it doesn't fit. */
	if (*num != u64)
		return false;
	return true;
}

bool json_to_int(const char *buffer, const jsmntok_t *tok, int *num)
{
	s64 tmp;

	if (!json_to_s64(buffer, tok, &tmp))
		return false;
	*num = tmp;

	/* Just in case it doesn't fit. */
	if (*num != tmp)
		return false;

	return true;
}

bool json_to_errcode(const char *buffer, const jsmntok_t *tok, errcode_t *errcode)
{
	s64 tmp;

	if (!json_to_s64(buffer, tok, &tmp))
		return false;
	*errcode = tmp;

	/* Just in case it doesn't fit. */
	if (*errcode != tmp)
		return false;

	return true;
}

bool json_to_sha256(const char *buffer, const jsmntok_t *tok, struct sha256 *dest)
{
	if (tok->type != JSMN_STRING)
		return false;

	return hex_decode(buffer + tok->start, tok->end - tok->start, dest,
			  sizeof(struct sha256));
}

u8 *json_tok_bin_from_hex(const tal_t *ctx, const char *buffer, const jsmntok_t *tok)
{
	u8 *result;
	size_t hexlen, rawlen;
	hexlen = tok->end - tok->start;
	rawlen = hex_data_size(hexlen);

	result = tal_arr(ctx, u8, rawlen);
	if (!hex_decode(buffer + tok->start, hexlen, result, rawlen))
		return tal_free(result);

	return result;
}

/* talfmt take a ctx pointer and return NULL or a valid pointer.
 * fmt takes the argument, and returns a bool.
 *
 * This function returns NULL on success, or errmsg on failure.
*/
static const char *handle_percent(const char *buffer,
				  const jsmntok_t *tok,
				  va_list *ap)
{
	void *ctx;
	const char *fmtname;

	/* This is set to (dummy) json_scan if it's a non-tal fmt */
	ctx = va_arg(*ap, void *);
	fmtname = va_arg(*ap, const char *);
	if (ctx != json_scan) {
		void *(*talfmt)(void *, const char *, const jsmntok_t *);
		void **p;
		p = va_arg(*ap, void **);
		talfmt = va_arg(*ap, void *(*)(void *, const char *, const jsmntok_t *));
		*p = talfmt(ctx, buffer, tok);
		if (*p != NULL)
			return NULL;
	} else {
		bool (*fmt)(const char *, const jsmntok_t *, void *);
		void *p;

		p = va_arg(*ap, void *);
		fmt = va_arg(*ap, bool (*)(const char *, const jsmntok_t *, void *));
		if (fmt(buffer, tok, p))
			return NULL;
	}

	return tal_fmt(tmpctx, "%s could not parse %.*s",
		       fmtname,
		       json_tok_full_len(tok),
		       json_tok_full(buffer, tok));
}

/* GUIDE := OBJ | ARRAY | '%'
 * OBJ := '{' FIELDLIST '}'
 * FIELDLIST := FIELD [',' FIELD]*
 * FIELD := LITERAL ':' FIELDVAL
 * FIELDVAL := OBJ | ARRAY | LITERAL | '%'
 * ARRAY := '[' ARRLIST ']'
 * ARRLIST := ARRELEM [',' ARRELEM]*
 * ARRELEM := NUMBER ':' FIELDVAL
 */

static void parse_literal(const char **guide,
			  const char **literal,
			  size_t *len)
{
	*literal = *guide;
	*len = strspn(*guide,
		      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		      "abcdefghijklmnopqrstuvwxyz"
		      "0123456789"
		      "_-");
	*guide += *len;
}

static void parse_number(const char **guide, u32 *number)
{
	char *endp;
	long int l;

	l = strtol(*guide, &endp, 10);
	assert(endp != *guide);
	assert(errno != ERANGE);

	/* Test for overflow */
	*number = l;
	assert(*number == l);

	*guide = endp;
}

static char guide_consume_one(const char **guide)
{
	char c = **guide;
	(*guide)++;
	return c;
}

static void guide_must_be(const char **guide, char c)
{
	char actual = guide_consume_one(guide);
	assert(actual == c);
}

/* Recursion: return NULL on success, errmsg on fail */
static const char *parse_obj(const char *buffer,
			     const jsmntok_t *tok,
			     const char **guide,
			     va_list *ap);

static const char *parse_arr(const char *buffer,
			     const jsmntok_t *tok,
			     const char **guide,
			     va_list *ap);

static const char *parse_guide(const char *buffer,
			       const jsmntok_t *tok,
			       const char **guide,
			       va_list *ap)
{
	const char *errmsg;

	if (**guide == '{') {
		errmsg = parse_obj(buffer, tok, guide, ap);
		if (errmsg)
			return errmsg;
	} else if (**guide == '[') {
		errmsg = parse_arr(buffer, tok, guide, ap);
		if (errmsg)
			return errmsg;
	} else {
		guide_must_be(guide, '%');
		errmsg = handle_percent(buffer, tok, ap);
		if (errmsg)
			return errmsg;
	}
	return NULL;
}

static const char *parse_fieldval(const char *buffer,
				  const jsmntok_t *tok,
				  const char **guide,
				  va_list *ap)
{
	const char *errmsg;

	if (**guide == '{') {
		errmsg = parse_obj(buffer, tok, guide, ap);
		if (errmsg)
			return errmsg;
	} else if (**guide == '[') {
		errmsg = parse_arr(buffer, tok, guide, ap);
		if (errmsg)
			return errmsg;
	} else if (**guide == '%') {
		guide_consume_one(guide);
		errmsg = handle_percent(buffer, tok, ap);
		if (errmsg)
			return errmsg;
	} else {
		const char *literal;
		size_t len;

		/* Literal must match exactly (modulo quotes for strings) */
		parse_literal(guide, &literal, &len);
		if (!memeq(buffer + tok->start, tok->end - tok->start,
			   literal, len)) {
			return tal_fmt(tmpctx,
				       "%.*s does not match expected %.*s",
				       json_tok_full_len(tok),
				       json_tok_full(buffer, tok),
				       (int)len, literal);
		}
	}
	return NULL;
}

static const char *parse_field(const char *buffer,
			       const jsmntok_t *tok,
			       const char **guide,
			       va_list *ap)
{
	const jsmntok_t *member;
	size_t len;
	const char *memname;

	parse_literal(guide, &memname, &len);
	guide_must_be(guide, ':');

	member = json_get_membern(buffer, tok, memname, len);
	if (!member) {
		return tal_fmt(tmpctx, "object does not have member %.*s",
			       (int)len, memname);
	}

	return parse_fieldval(buffer, member, guide, ap);
}

static const char *parse_fieldlist(const char *buffer,
				   const jsmntok_t *tok,
				   const char **guide,
				   va_list *ap)
{
	for (;;) {
		const char *errmsg;

		errmsg = parse_field(buffer, tok, guide, ap);
		if (errmsg)
			return errmsg;
		if (**guide != ',')
			break;
		guide_consume_one(guide);
	}
	return NULL;
}

static const char *parse_obj(const char *buffer,
			     const jsmntok_t *tok,
			     const char **guide,
			     va_list *ap)
{
	const char *errmsg;

	guide_must_be(guide, '{');

	if (tok->type != JSMN_OBJECT) {
		return tal_fmt(tmpctx, "token is not an object: %.*s",
			       json_tok_full_len(tok),
			       json_tok_full(buffer, tok));
	}

	errmsg = parse_fieldlist(buffer, tok, guide, ap);
	if (errmsg)
		return errmsg;

	guide_must_be(guide, '}');
	return NULL;
}

static const char *parse_arrelem(const char *buffer,
				 const jsmntok_t *tok,
				 const char **guide,
				 va_list *ap)
{
	const jsmntok_t *member;
	u32 idx;

	parse_number(guide, &idx);
	guide_must_be(guide, ':');

	member = json_get_arr(tok, idx);
	if (!member) {
		return tal_fmt(tmpctx, "token has no index %u: %.*s",
			       idx,
			       json_tok_full_len(tok),
			       json_tok_full(buffer, tok));
	}

	return parse_fieldval(buffer, member, guide, ap);
}

static const char *parse_arrlist(const char *buffer,
				 const jsmntok_t *tok,
				 const char **guide,
				 va_list *ap)
{
	const char *errmsg;

	for (;;) {
		errmsg = parse_arrelem(buffer, tok, guide, ap);
		if (errmsg)
			return errmsg;
		if (**guide != ',')
			break;
		guide_consume_one(guide);
	}
	return NULL;
}

static const char *parse_arr(const char *buffer,
			     const jsmntok_t *tok,
			     const char **guide,
			     va_list *ap)
{
	const char *errmsg;

	guide_must_be(guide, '[');

	if (tok->type != JSMN_ARRAY) {
		return tal_fmt(tmpctx, "token is not an array: %.*s",
			       json_tok_full_len(tok),
			       json_tok_full(buffer, tok));
	}

	errmsg = parse_arrlist(buffer, tok, guide, ap);
	if (errmsg)
		return errmsg;

	guide_must_be(guide, ']');
	return NULL;
}

const char *json_scanv(const tal_t *ctx,
		       const char *buffer,
		       const jsmntok_t *tok,
		       const char *guide,
		       va_list ap)
{
	va_list cpy;
	const char *orig_guide = guide, *errmsg;

	/* We need this, since &ap doesn't work on some platforms... */
	va_copy(cpy, ap);
	errmsg = parse_guide(buffer, tok, &guide, &cpy);
	va_end(cpy);

	if (errmsg) {
		return tal_fmt(ctx, "Parsing '%.*s': %s",
			       (int)(guide - orig_guide), orig_guide,
			       errmsg);
	}
	assert(guide[0] == '\0');
	return NULL;
}

const char *json_scan(const tal_t *ctx,
		      const char *buffer,
		      const jsmntok_t *tok,
		      const char *guide,
		      ...)
{
	va_list ap;
	const char *ret;

	va_start(ap, guide);
	ret = json_scanv(ctx, buffer, tok, guide, ap);
	va_end(ap);
	return ret;
}

bool json_to_bitcoin_amount(const char *buffer, const jsmntok_t *tok,
			    uint64_t *satoshi)
{
	char *end;
	unsigned long btc, sat;

	btc = strtoul(buffer + tok->start, &end, 10);
	if (btc == ULONG_MAX && errno == ERANGE)
		return false;
	if (end != buffer + tok->end) {
		/* Expect always 8 decimal places. */
		if (*end != '.' || buffer + tok->end - end != 9)
			return false;
		sat = strtoul(end+1, &end, 10);
		if (sat == ULONG_MAX && errno == ERANGE)
			return false;
		if (end != buffer + tok->end)
			return false;
	} else
		sat = 0;

	*satoshi = btc * (uint64_t)100000000 + sat;
	if (*satoshi != btc * (uint64_t)100000000 + sat)
		return false;

	return true;
}

bool json_to_node_id(const char *buffer, const jsmntok_t *tok,
		     struct node_id *id)
{
	return node_id_from_hexstr(buffer + tok->start,
				   tok->end - tok->start, id);
}

bool json_to_pubkey(const char *buffer, const jsmntok_t *tok,
		    struct pubkey *pubkey)
{
	return pubkey_from_hexstr(buffer + tok->start,
				  tok->end - tok->start, pubkey);
}

bool json_to_msat(const char *buffer, const jsmntok_t *tok,
		  struct amount_msat *msat)
{
	return parse_amount_msat(msat,
				 buffer + tok->start, tok->end - tok->start);
}

bool json_to_sat(const char *buffer, const jsmntok_t *tok,
		 struct amount_sat *sat)
{
	return parse_amount_sat(sat, buffer + tok->start, tok->end - tok->start);
}

bool json_to_sat_or_all(const char *buffer, const jsmntok_t *tok,
			struct amount_sat *sat)
{
	if (json_tok_streq(buffer, tok, "all")) {
		*sat = AMOUNT_SAT(-1ULL);
		return true;
	}
	return json_to_sat(buffer, tok, sat);
}

bool json_to_short_channel_id(const char *buffer, const jsmntok_t *tok,
			      struct short_channel_id *scid)
{
	return (short_channel_id_from_str(buffer + tok->start,
					  tok->end - tok->start, scid));
}

bool json_to_txid(const char *buffer, const jsmntok_t *tok,
		  struct bitcoin_txid *txid)
{
	return bitcoin_txid_from_hex(buffer + tok->start,
				     tok->end - tok->start, txid);
}

bool json_to_outpoint(const char *buffer, const jsmntok_t *tok,
		      struct bitcoin_outpoint *op)
{
	jsmntok_t t1, t2;

	if (!split_tok(buffer, tok, ':', &t1, &t2))
		return false;

	return json_to_txid(buffer, &t1, &op->txid)
		&& json_to_u32(buffer, &t2, &op->n);
}

bool json_to_channel_id(const char *buffer, const jsmntok_t *tok,
			struct channel_id *cid)
{
	return hex_decode(buffer + tok->start, tok->end - tok->start,
			  cid, sizeof(*cid));
}

bool json_to_coin_mvt_tag(const char *buffer, const jsmntok_t *tok,
			  enum mvt_tag *tag)
{
	enum mvt_tag i_tag;
	for (size_t i = 0; i < NUM_MVT_TAGS; i++) {
		i_tag = (enum mvt_tag) i;
		if (json_tok_streq(buffer, tok, mvt_tag_str(i_tag))) {
			*tag = i_tag;
			return true;
		}
	}

	return false;
}

bool split_tok(const char *buffer, const jsmntok_t *tok,
				char split,
				jsmntok_t *a,
				jsmntok_t *b)
{
	const char *p = memchr(buffer + tok->start, split, tok->end - tok->start);
	if (!p)
		return false;

	*a = *b = *tok;
	a->end = p - buffer;
	b->start = p + 1 - buffer;

	return true;
}

bool json_to_secret(const char *buffer, const jsmntok_t *tok, struct secret *dest)
{
	return hex_decode(buffer + tok->start, tok->end - tok->start,
			  dest->data, sizeof(struct secret));
}

bool json_to_preimage(const char *buffer, const jsmntok_t *tok, struct preimage *preimage)
{
	size_t hexlen = tok->end - tok->start;
	return hex_decode(buffer + tok->start, hexlen, preimage->r, sizeof(preimage->r));
}

struct wally_psbt *json_to_psbt(const tal_t *ctx, const char *buffer,
				const jsmntok_t *tok)
{
	return psbt_from_b64(ctx, buffer + tok->start, tok->end - tok->start);
}

struct tlv_obs2_onionmsg_payload_reply_path *
json_to_obs2_reply_path(const tal_t *ctx, const char *buffer, const jsmntok_t *tok)
{
	struct tlv_obs2_onionmsg_payload_reply_path *rpath;
	const jsmntok_t *hops, *t;
	size_t i;
	const char *err;

	rpath = tal(ctx, struct tlv_obs2_onionmsg_payload_reply_path);
	err = json_scan(tmpctx, buffer, tok, "{blinding:%,first_node_id:%}",
			JSON_SCAN(json_to_pubkey, &rpath->blinding),
			JSON_SCAN(json_to_pubkey, &rpath->first_node_id),
			NULL);
	if (err)
		return tal_free(rpath);

	hops = json_get_member(buffer, tok, "hops");
	if (!hops || hops->size < 1)
		return tal_free(rpath);

	rpath->path = tal_arr(rpath, struct onionmsg_path *, hops->size);
	json_for_each_arr(i, t, hops) {
		rpath->path[i] = tal(rpath->path, struct onionmsg_path);
		err = json_scan(tmpctx, buffer, t, "{id:%,encrypted_recipient_data:%}",
				JSON_SCAN(json_to_pubkey,
					  &rpath->path[i]->node_id),
				JSON_SCAN_TAL(rpath->path[i],
					      json_tok_bin_from_hex,
					      &rpath->path[i]->encrypted_recipient_data));
		if (err)
			return tal_free(rpath);
	}

	return rpath;
}

struct tlv_onionmsg_payload_reply_path *
json_to_reply_path(const tal_t *ctx, const char *buffer, const jsmntok_t *tok)
{
	struct tlv_onionmsg_payload_reply_path *rpath;
	const jsmntok_t *hops, *t;
	size_t i;
	const char *err;

	rpath = tal(ctx, struct tlv_onionmsg_payload_reply_path);
	err = json_scan(tmpctx, buffer, tok, "{blinding:%,first_node_id:%}",
			JSON_SCAN(json_to_pubkey, &rpath->blinding),
			JSON_SCAN(json_to_pubkey, &rpath->first_node_id),
			NULL);
	if (err)
		return tal_free(rpath);

	hops = json_get_member(buffer, tok, "hops");
	if (!hops || hops->size < 1)
		return tal_free(rpath);

	rpath->path = tal_arr(rpath, struct onionmsg_path *, hops->size);
	json_for_each_arr(i, t, hops) {
		rpath->path[i] = tal(rpath->path, struct onionmsg_path);
		err = json_scan(tmpctx, buffer, t, "{id:%,encrypted_recipient_data:%}",
				JSON_SCAN(json_to_pubkey,
					  &rpath->path[i]->node_id),
				JSON_SCAN_TAL(rpath->path[i],
					      json_tok_bin_from_hex,
					      &rpath->path[i]->encrypted_recipient_data));
		if (err)
			return tal_free(rpath);
	}

	return rpath;
}


bool
json_tok_channel_id(const char *buffer, const jsmntok_t *tok,
		    struct channel_id *cid)
{
	return hex_decode(buffer + tok->start, tok->end - tok->start,
			  cid, sizeof(*cid));
}
