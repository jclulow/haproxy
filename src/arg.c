/*
 * Functions used to parse typed argument lists
 *
 * Copyright 2012 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <common/standard.h>
#include <proto/arg.h>

static const char *arg_type_names[ARGT_NBTYPES] = {
	[ARGT_STOP] = "end of arguments",
	[ARGT_UINT] = "unsigned integer",
	[ARGT_SINT] = "signed integer",
	[ARGT_STR]  = "string",
	[ARGT_IPV4] = "IPv4 address",
	[ARGT_MSK4] = "IPv4 mask",
	[ARGT_IPV6] = "IPv6 address",
	[ARGT_MSK6] = "IPv6 mask",
	[ARGT_TIME] = "delay",
	[ARGT_SIZE] = "size",
	[ARGT_FE]   = "frontend",
	[ARGT_BE]   = "backend",
	[ARGT_TAB]  = "table",
	[ARGT_SRV]  = "server",
	[ARGT_USR]  = "user list",
	/* Unassigned types must never happen. Better crash during parsing if they do. */
};

/* This function builds an argument list from a config line. It returns the
 * number of arguments found, or <0 in case of any error. Everything needed
 * it automatically allocated. A pointer to an error message might be returned
 * in err_msg if not NULL, in which case it would be allocated and the caller
 * will have to check it and free it. The output arg list is returned in argp
 * which must be valid. The returned array is always terminated by an arg of
 * type ARGT_STOP (0), unless the mask indicates that no argument is supported.
 * The mask is composed of a number of mandatory arguments in its lower 4 bits,
 * and a concatenation of each argument type in each subsequent 4-bit block. If
 * <err_msg> is not NULL, it must point to a freeable or NULL pointer.
 */
int make_arg_list(const char *in, int len, unsigned int mask, struct arg **argp,
		  char **err_msg, const char **err_ptr, int *err_arg)
{
	int nbarg;
	int pos;
	struct arg *arg, *arg_list = NULL;
	const char *beg;
	char *word = NULL;
	const char *ptr_err = NULL;
	int min_arg;

	min_arg = mask & 15;
	mask >>= 4;

	pos = 0;
	/* find between 0 and 8 the max number of args supported by the mask */
	for (nbarg = 0; nbarg < 8 && ((mask >> (nbarg * 4)) & 0xF); nbarg++);

	if (!nbarg)
		goto end_parse;

	/* Note: an empty input string contains an empty argument if this argument
	 * is marked mandatory. Otherwise we can ignore it.
	 */
	if (!len && !min_arg)
		goto end_parse;

	arg = arg_list = calloc(nbarg + 1, sizeof(*arg));

	/* Note: empty arguments after a comma always exist. */
	while (pos < nbarg) {
		beg = in;
		while (len && *in != ',') {
			in++;
			len--;
		}

		/* we have a new argument between <beg> and <in> (not included).
		 * For ease of handling, we copy it into a zero-terminated word.
		 * By default, the output argument will be the same type of the
		 * expected one.
		 */
		free(word);
		word = my_strndup(beg, in - beg);

		arg->type = (mask >> (pos * 4)) & 15;

		switch (arg->type) {
		case ARGT_SINT:
			if (in == beg)	  // empty number
				goto parse_err;
			else if (*beg < '0' || *beg > '9') {
				arg->data.sint = strl2uic(beg + 1, in - beg - 1);
				if (*beg == '-')
					arg->data.sint = -arg->data.sint;
				else if (*beg != '+')    // invalid first character
					goto parse_err;
				break;
			}

			arg->type = ARGT_UINT;
			/* fall through ARGT_UINT if no sign is present */

		case ARGT_UINT:
			if (in == beg)    // empty number
				goto parse_err;

			arg->data.uint = strl2uic(beg, in - beg);
			break;

		case ARGT_FE:
		case ARGT_BE:
		case ARGT_TAB:
		case ARGT_SRV:
		case ARGT_USR:
		case ARGT_STR:
			/* all types that must be resolved are stored as strings
			 * during the parsing. The caller must at one point resolve
			 * them and free the string.
			 */
			arg->data.str.str = word;
			arg->data.str.len = in - beg;
			arg->data.str.size = arg->data.str.len + 1;
			word = NULL;
			break;

		case ARGT_IPV4:
			if (in == beg)    // empty address
				goto parse_err;

			if (inet_pton(AF_INET, word, &arg->data.ipv4) <= 0)
				goto parse_err;
			break;

		case ARGT_MSK4:
			if (in == beg)    // empty mask
				goto parse_err;

			if (!str2mask(word, &arg->data.ipv4))
				goto parse_err;

			arg->type = ARGT_IPV4;
			break;

		case ARGT_IPV6:
			if (in == beg)    // empty address
				goto parse_err;

			if (inet_pton(AF_INET6, word, &arg->data.ipv6) <= 0)
				goto parse_err;
			break;

		case ARGT_MSK6: /* not yet implemented */
			goto parse_err;

		case ARGT_TIME:
			if (in == beg)    // empty time
				goto parse_err;

			ptr_err = parse_time_err(word, &arg->data.uint, TIME_UNIT_MS);
			if (ptr_err)
				goto parse_err;

			arg->type = ARGT_UINT;
			break;

		case ARGT_SIZE:
			if (in == beg)    // empty size
				goto parse_err;

			ptr_err = parse_size_err(word, &arg->data.uint);
			if (ptr_err)
				goto parse_err;

			arg->type = ARGT_UINT;
			break;

			/* FIXME: other types need to be implemented here */
		default:
			goto parse_err;
		}

		pos++;
		arg++;

		/* don't go back to parsing if we reached end */
		if (!len || pos >= nbarg)
			break;

		/* skip comma */
		in++; len--;
	}

 end_parse:
	free(word); word = NULL;

	if (pos < min_arg) {
		/* not enough arguments */
		if (err_msg)
			memprintf(err_msg,
				  "Missing arguments (got %d/%d), type '%s' expected",
				  pos, min_arg, arg_type_names[(mask >> (pos * 4)) & 15]);
		goto err;
	}

	if (len) {
		/* too many arguments, starting at <in> */
		if (err_msg) {
			/* the caller is responsible for freeing this message */
			word = my_strndup(in, len);
			memprintf(err_msg, "End of arguments expected at '%s'", word);
			free(word); word = NULL;
		}
		goto err;
	}

	/* note that pos might be < nbarg and this is not an error, it's up to the
	 * caller to decide what to do with optional args.
	 */
	*argp = arg_list;

	if (err_arg)
		*err_arg = pos;
	if (err_ptr)
		*err_ptr = in;
	return pos;

 parse_err:
	if (err_msg) {
		memprintf(err_msg, "Failed to parse '%s' as type '%s'",
			  word, arg_type_names[(mask >> (pos * 4)) & 15]);
	}

 err:
	free(word);
	free(arg_list);
	if (err_arg)
		*err_arg = pos;
	if (err_ptr)
		*err_ptr = in;
	return -1;
}