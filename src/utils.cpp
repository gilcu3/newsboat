#include "utils.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <cwchar>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <langinfo.h>
#include <libxml/uri.h>
#include <locale>
#include <mutex>
#include <pwd.h>
#include <regex>
#include <sstream>
#include <stfl.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <unordered_set>

#include "config.h"
#include "htmlrenderer.h"
#include "logger.h"
#include "ruststring.h"
#include "strprintf.h"

#if HAVE_GCRYPT
#include <errno.h>
#include <gcrypt.h>
#include <gnutls/gnutls.h>
#include <pthread.h>
GCRY_THREAD_OPTION_PTHREAD_IMPL;
#endif

#if HAVE_OPENSSL && OPENSSL_VERSION_NUMBER < 0x01010000fL
#include <openssl/crypto.h>
#endif

#include "rs_utils.h"

using HTTPMethod = newsboat::utils::HTTPMethod;

namespace newsboat {

namespace utils {
void append_escapes(std::string& str, char c)
{
	switch (c) {
	case 'n':
		str.append("\n");
		break;
	case 'r':
		str.append("\r");
		break;
	case 't':
		str.append("\t");
		break;
	case '"':
		str.append("\"");
		break;
	// escaped backticks are passed through, still escaped. We un-escape
	// them in ConfigParser::evaluate_backticks
	case '`':
		str.append("\\`");
		break;
	case '\\':
		str.append("\\");
		break;
	default:
		str.push_back(c);
		break;
	}
}
}

std::string utils::strip_comments(const std::string& line)
{
	return std::string(utils::bridged::strip_comments(line));
}

std::vector<std::string> utils::tokenize_quoted(const std::string& str,
	std::string delimiters)
{
	/*
	 * This function tokenizes strings, obeying quotes and throwing away
	 * comments that start with a '#'.
	 *
	 * e.g. line: foo bar "foo bar" "a test"
	 * is parsed to 4 elements:
	 * 	[0]: foo
	 * 	[1]: bar
	 * 	[2]: foo bar
	 * 	[3]: a test
	 *
	 * e.g. line: yes great "x\ny" # comment
	 * is parsed to 3 elements:
	 * 	[0]: yes
	 * 	[1]: great
	 * 	[2]: x
	 * 	y
	 *
	 * 	\", \r, \n, and \t are replaced with the literals that you
	 * know from C/C++ strings.
	 *
	 */
	std::vector<std::string> tokens;
	std::string remaining = str;
	while (!remaining.empty()) {
		auto token = extract_token_quoted(remaining, delimiters);
		if (token.has_value()) {
			tokens.push_back(token.value());
		}
	}

	return tokens;
}

nonstd::optional<std::string> utils::extract_token_quoted(std::string& str,
	std::string delimiters)
{
	auto first_non_delimiter = str.find_first_not_of(delimiters, 0);
	if (first_non_delimiter == std::string::npos) {
		str = "";
		return {};
	}
	str = str.substr(first_non_delimiter);

	if (str[0] == '#') { // stop as soon as we find a comment
		str = "";
		return {};
	}

	std::string token;
	if (str[0] == '"') {
		std::string::size_type pos = 1;
		while (pos < str.length()) {
			if (str[pos] == '"') {
				// We've reached the end of this quoted token.
				++pos;
				break;
			} else if (str[pos] == '\\') {
				pos += 1;
				if (pos < str.length()) {
					append_escapes(token, str[pos]);
					pos += 1;
				}
			} else {
				token.push_back(str[pos]);
				++pos;
			}
		}
		str = str.substr(pos);
	} else {
		auto end_of_token = str.find_first_of(delimiters);
		token = str.substr(0, end_of_token);
		if (end_of_token == std::string::npos) {
			str = "";
		} else {
			str = str.substr(end_of_token);
		}
	}

	return token;
}

std::vector<std::string> utils::tokenize(const std::string& str,
	std::string delimiters)
{
	/*
	 * This function tokenizes a string by the delimiters. Plain and simple.
	 */
	std::vector<std::string> tokens;
	std::string::size_type last_pos = str.find_first_not_of(delimiters, 0);
	std::string::size_type pos = str.find_first_of(delimiters, last_pos);

	while (std::string::npos != pos || std::string::npos != last_pos) {
		tokens.push_back(str.substr(last_pos, pos - last_pos));
		last_pos = str.find_first_not_of(delimiters, pos);
		pos = str.find_first_of(delimiters, last_pos);
	}
	return tokens;
}

std::vector<std::string> utils::tokenize_spaced(const std::string& str,
	std::string delimiters)
{
	std::vector<std::string> tokens;
	std::string::size_type last_pos = str.find_first_not_of(delimiters, 0);
	std::string::size_type pos = str.find_first_of(delimiters, last_pos);

	if (last_pos != 0) {
		tokens.push_back(str.substr(0, last_pos));
	}

	while (std::string::npos != pos || std::string::npos != last_pos) {
		tokens.push_back(str.substr(last_pos, pos - last_pos));
		last_pos = str.find_first_not_of(delimiters, pos);
		if (last_pos > pos) {
			tokens.push_back(str.substr(pos, last_pos - pos));
		}
		pos = str.find_first_of(delimiters, last_pos);
	}

	return tokens;
}

std::string utils::consolidate_whitespace(const std::string& str)
{

	return std::string(utils::bridged::consolidate_whitespace(str));
}

std::vector<std::string> utils::tokenize_nl(const std::string& str,
	std::string delimiters)
{
	std::vector<std::string> tokens;
	std::string::size_type last_pos = str.find_first_not_of(delimiters, 0);
	std::string::size_type pos = str.find_first_of(delimiters, last_pos);
	unsigned int i;

	LOG(Level::DEBUG,
		"utils::tokenize_nl: last_pos = %" PRIu64,
		static_cast<uint64_t>(last_pos));
	if (last_pos != std::string::npos) {
		for (i = 0; i < last_pos; ++i) {
			tokens.push_back(std::string("\n"));
		}
	} else {
		for (i = 0; i < str.length(); ++i) {
			tokens.push_back(std::string("\n"));
		}
	}

	while (std::string::npos != pos || std::string::npos != last_pos) {
		tokens.push_back(str.substr(last_pos, pos - last_pos));
		LOG(Level::DEBUG,
			"utils::tokenize_nl: substr = %s",
			str.substr(last_pos, pos - last_pos));
		last_pos = str.find_first_not_of(delimiters, pos);
		LOG(Level::DEBUG,
			"utils::tokenize_nl: pos - last_pos = %" PRIu64,
			static_cast<uint64_t>(last_pos - pos));
		for (i = 0; last_pos != std::string::npos &&
			pos != std::string::npos && i < (last_pos - pos);
			++i) {
			tokens.push_back(std::string("\n"));
		}
		pos = str.find_first_of(delimiters, last_pos);
	}

	return tokens;
}

std::string utils::translit(const std::string& tocode,
	const std::string& fromcode)
{
	std::string tlit = "//TRANSLIT";

	enum class TranslitState { UNKNOWN, SUPPORTED, UNSUPPORTED };

	static TranslitState state = TranslitState::UNKNOWN;

	// TRANSLIT is not needed when converting to unicode encodings
	if (tocode == "utf-8" || tocode == "WCHAR_T") {
		return tocode;
	}

	if (state == TranslitState::UNKNOWN) {
		iconv_t cd = ::iconv_open(
				(tocode + "//TRANSLIT").c_str(), fromcode.c_str());

		if (cd == reinterpret_cast<iconv_t>(-1)) {
			if (errno == EINVAL) {
				iconv_t cd = ::iconv_open(
						tocode.c_str(), fromcode.c_str());
				if (cd != reinterpret_cast<iconv_t>(-1)) {
					state = TranslitState::UNSUPPORTED;
				} else {
					fprintf(stderr,
						"iconv_open('%s', '%s') "
						"failed: %s",
						tocode.c_str(),
						fromcode.c_str(),
						strerror(errno));
					abort();
				}
			} else {
				fprintf(stderr,
					"iconv_open('%s//TRANSLIT', '%s') "
					"failed: %s",
					tocode.c_str(),
					fromcode.c_str(),
					strerror(errno));
				abort();
			}
		} else {
			state = TranslitState::SUPPORTED;
		}

		iconv_close(cd);
	}

	return ((state == TranslitState::SUPPORTED) ? (tocode + tlit)
			: (tocode));
}

std::string utils::convert_text(const std::string& text,
	const std::string& tocode,
	const std::string& fromcode)
{
	std::string result;

	if (strcasecmp(tocode.c_str(), fromcode.c_str()) == 0) {
		return text;
	}

	iconv_t cd = ::iconv_open(
			translit(tocode, fromcode).c_str(), fromcode.c_str());

	if (cd == reinterpret_cast<iconv_t>(-1)) {
		return result;
	}

	size_t inbytesleft;
	size_t outbytesleft;

	/*
	 * of all the Unix-like systems around there, only Linux/glibc seems to
	 * come with a SuSv3-conforming iconv implementation.
	 */
#if !defined(__linux__) && !defined(__GLIBC__) && !defined(__APPLE__) && \
	!defined(__OpenBSD__) && !defined(__FreeBSD__) &&                \
	!defined(__DragonFly__)
	const char* inbufp;
#else
	char* inbufp;
#endif
	char outbuf[16];
	char* outbufp = outbuf;

	outbytesleft = sizeof(outbuf);
	inbufp = const_cast<char*>(
			text.c_str()); // evil, but spares us some trouble
	inbytesleft = strlen(inbufp);

	do {
		char* old_outbufp = outbufp;
		int rc = ::iconv(
				cd, &inbufp, &inbytesleft, &outbufp, &outbytesleft);
		if (-1 == rc) {
			switch (errno) {
			case E2BIG:
				result.append(
					old_outbufp, outbufp - old_outbufp);
				outbufp = outbuf;
				outbytesleft = sizeof(outbuf);
				inbufp += strlen(inbufp) - inbytesleft;
				inbytesleft = strlen(inbufp);
				break;
			case EILSEQ:
			case EINVAL:
				result.append(
					old_outbufp, outbufp - old_outbufp);
				result.append("?");
				inbufp += strlen(inbufp) - inbytesleft + 1;
				inbytesleft = strlen(inbufp);
				break;
			default:
				break;
			}
		} else {
			result.append(old_outbufp, outbufp - old_outbufp);
		}
	} while (inbytesleft > 0);

	iconv_close(cd);

	return result;
}

std::string utils::utf8_to_locale(const std::string& text)
{
	if (text.empty()) {
		return {};
	}

	return utils::convert_text(text, nl_langinfo(CODESET), "utf-8");
}

std::string utils::get_command_output(const std::string& cmd)
{
	return std::string(utils::bridged::get_command_output(cmd));
}

static size_t my_write_data(void* buffer, size_t size, size_t nmemb,
	void* userp)
{
	std::string* pbuf = static_cast<std::string*>(userp);
	pbuf->append(static_cast<const char*>(buffer), size * nmemb);
	return size * nmemb;
}

std::string utils::http_method_str(const HTTPMethod method)
{
	std::string str = "";

	switch (method) {
	case HTTPMethod::GET:
		str = "GET";
		break;
	case HTTPMethod::POST:
		str = "POST";
		break;
	case HTTPMethod::PUT:
		str = "PUT";
		break;
	case HTTPMethod::DELETE:
		str = "DELETE";
		break;
	}

	return str;
}

std::string utils::retrieve_url(const std::string& url,
	ConfigContainer* cfgcont,
	const std::string& authinfo,
	const std::string* body,
	const HTTPMethod method, /* = GET */
	CURL* cached_handle)
{
	std::string buf;

	CURL* easyhandle;
	if (cached_handle) {
		easyhandle = cached_handle;
	} else {
		easyhandle = curl_easy_init();
	}
	set_common_curl_options(easyhandle, cfgcont);
	curl_easy_setopt(easyhandle, CURLOPT_URL, url.c_str());
	curl_easy_setopt(easyhandle, CURLOPT_WRITEFUNCTION, my_write_data);
	curl_easy_setopt(easyhandle, CURLOPT_WRITEDATA, &buf);

	switch (method) {
	case HTTPMethod::GET:
		break;
	case HTTPMethod::POST:
		curl_easy_setopt(easyhandle, CURLOPT_POST, 1);
		break;
	case HTTPMethod::PUT:
	case HTTPMethod::DELETE:
		const std::string method_str = http_method_str(method);
		curl_easy_setopt(easyhandle, CURLOPT_CUSTOMREQUEST, method_str.c_str());
		break;
	}

	if (body != nullptr) {
		curl_easy_setopt(
			easyhandle, CURLOPT_POSTFIELDS, body->c_str());
	}

	if (!authinfo.empty()) {
		const auto auth_method = cfgcont->get_configvalue("http-auth-method");
		curl_easy_setopt(easyhandle, CURLOPT_HTTPAUTH, get_auth_method(auth_method));
		curl_easy_setopt(easyhandle, CURLOPT_USERPWD, authinfo.c_str());
	}

	curl_easy_perform(easyhandle);
	if (!cached_handle) {
		curl_easy_cleanup(easyhandle);
	}

	if (body != nullptr) {
		LOG(Level::DEBUG,
			"utils::retrieve_url(%s %s)[%s]: %s",
			http_method_str(method),
			url,
			body,
			buf);
	} else {
		LOG(Level::DEBUG, "utils::retrieve_url(%s)[-]: %s", url, buf);
	}

	return buf;
}

std::string utils::run_program(const char* argv[], const std::string& input)
{
	return RustString(rs_run_program(argv, input.c_str()));
}

std::string utils::resolve_tilde(const std::string& str)
{
	return std::string(utils::bridged::resolve_tilde(str));
}

std::string utils::resolve_relative(const std::string& reference,
	const std::string& fname)
{
	return std::string(utils::bridged::resolve_relative(reference, fname));
}

std::string utils::replace_all(std::string str,
	const std::string& from,
	const std::string& to)
{
	return std::string(utils::bridged::replace_all(str, from, to));
}

std::string utils::replace_all(const std::string& str,
	const std::vector<std::pair<std::string, std::string>> from_to_pairs)
{
	std::size_t cur_index = 0;
	std::string output;
	while (cur_index != str.size()) {
		std::size_t first_match = std::string::npos;
		std::pair<std::string, std::string> first_match_pair;
		for (const auto& p : from_to_pairs) {
			auto match = str.find(p.first, cur_index);
			if (match != std::string::npos && match < first_match) {
				first_match = match;
				first_match_pair = p;
			}
		}
		if (first_match == std::string::npos) {
			output += str.substr(cur_index);
			break;
		} else {
			output += str.substr(cur_index, first_match - cur_index);
			cur_index = first_match + first_match_pair.first.size();
			output += first_match_pair.second;
		}
	}
	return output;
}

std::wstring utils::str2wstr(const std::string& str)
{
	const char* codeset = nl_langinfo(CODESET);
	struct stfl_ipool* ipool = stfl_ipool_create(codeset);
	std::wstring result = stfl_ipool_towc(ipool, str.c_str());
	stfl_ipool_destroy(ipool);
	return result;
}

std::string utils::wstr2str(const std::wstring& wstr)
{
	std::string codeset = nl_langinfo(CODESET);
	codeset = translit(codeset, "WCHAR_T");
	struct stfl_ipool* ipool = stfl_ipool_create(codeset.c_str());
	std::string result = stfl_ipool_fromwc(ipool, wstr.c_str());
	stfl_ipool_destroy(ipool);
	return result;
}

std::string utils::absolute_url(const std::string& url, const std::string& link)
{
	return std::string(utils::bridged::absolute_url(url, link));
}

std::string utils::get_useragent(ConfigContainer* cfgcont)
{
	std::string ua_pref = cfgcont->get_configvalue("user-agent");
	if (ua_pref.length() == 0) {
		struct utsname buf;
		uname(&buf);
		if (strcmp(buf.sysname, "Darwin") == 0) {
			/* Assume it is a Mac from the last decade or at least
			 * Mac-like */
			const char* PROCESSOR = "";
			if (strcmp(buf.machine, "x86_64") == 0 ||
				strcmp(buf.machine, "i386") == 0) {
				PROCESSOR = "Intel ";
			}
			return strprintf::fmt("%s/%s (Macintosh; %sMac OS X)",
					PROGRAM_NAME,
					utils::program_version(),
					PROCESSOR);
		}
		return strprintf::fmt("%s/%s (%s %s)",
				PROGRAM_NAME,
				utils::program_version(),
				buf.sysname,
				buf.machine);
	}
	return ua_pref;
}

unsigned int utils::to_u(const std::string& str,
	const unsigned int default_value)
{
	return bridged::to_u(str, default_value);
}

std::vector<std::pair<unsigned int, unsigned int>> utils::partition_indexes(
		unsigned int start,
		unsigned int end,
		unsigned int parts)
{
	std::vector<std::pair<unsigned int, unsigned int>> partitions;
	unsigned int count = end - start + 1;
	unsigned int size = count / parts;

	for (unsigned int i = 0; i < parts - 1; i++) {
		partitions.push_back(std::pair<unsigned int, unsigned int>(
				start, start + size - 1));
		start += size;
	}

	partitions.push_back(std::pair<unsigned int, unsigned int>(start, end));
	return partitions;
}

std::string utils::substr_with_width(const std::string& str,
	const size_t max_width)
{
	return std::string(utils::bridged::substr_with_width(str, max_width));
}

std::string utils::substr_with_width_stfl(const std::string& str,
	const size_t max_width)
{
	return std::string(utils::bridged::substr_with_width_stfl(str, max_width));
}

std::string utils::join(const std::vector<std::string>& strings,
	const std::string& separator)
{
	std::string result;

	for (const auto& str : strings) {
		result.append(str);
		result.append(separator);
	}

	if (result.length() > 0)
		result.erase(
			result.length() - separator.length(), result.length());

	return result;
}

std::string utils::censor_url(const std::string& url)
{
	return std::string(utils::bridged::censor_url(url));
}

std::string utils::quote_for_stfl(std::string str)
{
	return std::string(utils::bridged::quote_for_stfl(str));
}

void utils::trim(std::string& str)
{
	str = std::string(utils::bridged::trim(str));
}

void utils::trim_end(std::string& str)
{
	str = std::string(utils::bridged::trim_end(str));
}

std::string utils::quote(const std::string& str)
{
	return std::string(utils::bridged::quote(str));
}

std::string utils::quote_if_necessary(const std::string& str)
{
	return std::string(utils::bridged::quote_if_necessary(str));
}

void utils::set_common_curl_options(CURL* handle, ConfigContainer* cfg)
{
	if (cfg) {
		if (cfg->get_configvalue_as_bool("use-proxy")) {
			const std::string proxy = cfg->get_configvalue("proxy");
			if (proxy != "")
				curl_easy_setopt(
					handle, CURLOPT_PROXY, proxy.c_str());

			const std::string proxyauth =
				cfg->get_configvalue("proxy-auth");
			const std::string proxyauthmethod =
				cfg->get_configvalue("proxy-auth-method");
			if (proxyauth != "") {
				curl_easy_setopt(handle,
					CURLOPT_PROXYAUTH,
					get_auth_method(proxyauthmethod));
				curl_easy_setopt(handle,
					CURLOPT_PROXYUSERPWD,
					proxyauth.c_str());
			}

			const std::string proxytype =
				cfg->get_configvalue("proxy-type");
			if (proxytype != "") {
				LOG(Level::DEBUG,
					"utils::set_common_curl_options: "
					"proxytype "
					"= %s",
					proxytype);
				curl_easy_setopt(handle,
					CURLOPT_PROXYTYPE,
					get_proxy_type(proxytype));
			}
		}

		const std::string useragent = utils::get_useragent(cfg);
		curl_easy_setopt(handle, CURLOPT_USERAGENT, useragent.c_str());

		const unsigned int dl_timeout =
			cfg->get_configvalue_as_int("download-timeout");
		curl_easy_setopt(handle, CURLOPT_TIMEOUT, dl_timeout);

		const std::string cookie_cache =
			cfg->get_configvalue("cookie-cache");
		if (cookie_cache != "") {
			curl_easy_setopt(handle,
				CURLOPT_COOKIEFILE,
				cookie_cache.c_str());
			curl_easy_setopt(handle,
				CURLOPT_COOKIEJAR,
				cookie_cache.c_str());
		}

		curl_easy_setopt(handle,
			CURLOPT_SSL_VERIFYHOST,
			cfg->get_configvalue_as_bool("ssl-verifyhost") ? 2 : 0);
		curl_easy_setopt(handle,
			CURLOPT_SSL_VERIFYPEER,
			cfg->get_configvalue_as_bool("ssl-verifypeer"));
	}

	curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(handle, CURLOPT_ENCODING, "gzip, deflate");

	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 10);
	curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1);

	const char* curl_ca_bundle = ::getenv("CURL_CA_BUNDLE");
	if (curl_ca_bundle != nullptr) {
		curl_easy_setopt(handle, CURLOPT_CAINFO, curl_ca_bundle);
	}
}

std::string utils::get_content(xmlNode* node)
{
	std::string retval;
	if (node) {
		xmlChar* content = xmlNodeGetContent(node);
		if (content) {
			retval = (const char*)content;
			xmlFree(content);
		}
	}
	return retval;
}

std::string utils::get_basename(const std::string& url)
{
	return std::string(utils::bridged::get_basename(url));
}

curl_proxytype utils::get_proxy_type(const std::string& type)
{
	if (type == "http") {
		return CURLPROXY_HTTP;
	}
	if (type == "socks4") {
		return CURLPROXY_SOCKS4;
	}
	if (type == "socks5") {
		return CURLPROXY_SOCKS5;
	}
	if (type == "socks5h") {
		return CURLPROXY_SOCKS5_HOSTNAME;
	}
#ifdef CURLPROXY_SOCKS4A
	if (type == "socks4a") {
		return CURLPROXY_SOCKS4A;
	}
#endif

	if (type != "") {
		LOG(Level::USERERROR,
			"you configured an invalid proxy type: %s",
			type);
	}
	return CURLPROXY_HTTP;
}

std::string utils::unescape_url(const std::string& url)
{
	bool success = false;
	const auto result = utils::bridged::unescape_url(url, success);
	if (!success) {
		LOG(Level::DEBUG, "Rust failed to unescape url: %s", url );
		throw std::runtime_error("unescaping url failed");
	} else {
		return std::string(result);
	}
}

std::wstring utils::clean_nonprintable_characters(std::wstring text)
{
	for (size_t idx = 0; idx < text.size(); ++idx) {
		if (!iswprint(text[idx])) {
			text[idx] = L'\uFFFD';
		}
	}
	return text;
}

/* Like mkdir(), but creates ancestors (parent directories) if they don't
 * exist. */
int utils::mkdir_parents(const std::string& p, mode_t mode)
{
	return utils::bridged::mkdir_parents(p, static_cast<std::uint32_t>(mode));
}

std::string utils::make_title(const std::string& const_url)
{
	return std::string(utils::bridged::make_title(const_url));
}

nonstd::optional<std::uint8_t> utils::run_interactively(
	const std::string& command,
	const std::string& caller)
{
	std::uint8_t exit_code = 0;
	if (bridged::run_interactively(command, caller, exit_code)) {
		return exit_code;
	}

	return nonstd::nullopt;
}

nonstd::optional<std::uint8_t> utils::run_non_interactively(
	const std::string& command,
	const std::string& caller)
{
	std::uint8_t exit_code = 0;
	if (bridged::run_non_interactively(command, caller, exit_code)) {
		return exit_code;
	}

	return nonstd::nullopt;
}

std::string utils::getcwd()
{
	return std::string(utils::bridged::getcwd());
}

utils::ReadTextFileResult utils::read_text_file( const std::string& filename)
{
	rust::Vec<rust::String> c;
	std::uint64_t error_line_number{};
	rust::String error_reason;
	const bool result = bridged::read_text_file(filename, c, error_line_number,
			error_reason);

	if (result) {
		std::vector<std::string> contents;
		for (const auto& line : c) {
			contents.push_back(std::string(line));
		}
		return contents;
	} else {
		ReadTextFileError error;

		if (error_line_number == 0) {
			error.kind = ReadTextFileErrorKind::CantOpen;
			error.message = strprintf::fmt(_("Failed to open file (%s)"),
					std::string(error_reason));
		} else {
			error.kind = ReadTextFileErrorKind::LineError;
			error.message = strprintf::fmt(_("Failed to read line %u (%s)"),
					error_line_number, std::string(error_reason));
		}

		return nonstd::make_unexpected(error);
	}
}

void utils::remove_soft_hyphens(std::string& text)
{
	rust::String tmp(text);
	utils::bridged::remove_soft_hyphens(tmp);
	text = std::string(tmp);
}

bool utils::is_valid_podcast_type(const std::string& mimetype)
{
	return rs_is_valid_podcast_type(mimetype.c_str());
}

nonstd::optional<LinkType> utils::podcast_mime_to_link_type(
	const std::string& mimetype)
{
	bool ok = false;
	const auto result = rs_podcast_mime_to_link_type(mimetype.c_str(), &ok);
	if (ok) {
		return static_cast<LinkType>(result);
	}

	return nonstd::nullopt;
}

/*
 * See
 * http://curl.haxx.se/libcurl/c/libcurl-tutorial.html#Multi-threading
 * for a reason why we do this.
 *
 * These callbacks are deprecated as of OpenSSL 1.1.0; see the
 * changelog: https://www.openssl.org/news/changelog.html#x6
 */

#if HAVE_OPENSSL && OPENSSL_VERSION_NUMBER < 0x01010000fL
static std::mutex* openssl_mutexes = nullptr;
static int openssl_mutexes_size = 0;

static void openssl_mth_locking_function(int mode, int n, const char* file,
	int line)
{
	if (n < 0 || n >= openssl_mutexes_size) {
		LOG(Level::ERROR,
			"openssl_mth_locking_function: index is out of bounds "
			"(called by %s:%d)",
			file,
			line);
		return;
	}
	if (mode & CRYPTO_LOCK) {
		LOG(Level::DEBUG, "OpenSSL lock %d: %s:%d", n, file, line);
		openssl_mutexes[n].lock();
	} else {
		LOG(Level::DEBUG, "OpenSSL unlock %d: %s:%d", n, file, line);
		openssl_mutexes[n].unlock();
	}
}

static unsigned long openssl_mth_id_function(void)
{
	return (unsigned long)pthread_self();
}
#endif

void utils::initialize_ssl_implementation(void)
{
#if HAVE_OPENSSL && OPENSSL_VERSION_NUMBER < 0x01010000fL
	openssl_mutexes_size = CRYPTO_num_locks();
	openssl_mutexes = new std::mutex[openssl_mutexes_size];
	CRYPTO_set_id_callback(openssl_mth_id_function);
	CRYPTO_set_locking_callback(openssl_mth_locking_function);
#endif

#if HAVE_GCRYPT
	gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
	gnutls_global_init();
#endif
}

std::string utils::get_default_browser()
{
	return std::string(utils::bridged::get_default_browser());
}

std::string utils::program_version()
{
	return std::string(utils::bridged::program_version());
}

std::string utils::mt_strf_localtime(const std::string& format, time_t t)
{
	// localtime() returns a pointer to static memory, so we need to protect
	// its caller with a mutex to ensure that no two threads concurrently
	// access that static memory. In Newsboat, the only caller for localtime()
	// is strftime(), that's why this function bakes the two together.

	static std::mutex mtx;
	std::lock_guard<std::mutex> guard(mtx);

	const size_t BUFFER_SIZE = 4096;
	char buffer[BUFFER_SIZE];
	const size_t written = strftime(buffer,
			BUFFER_SIZE,
			format.c_str(),
			localtime(&t));

	return std::string(buffer, written);
}

} // namespace newsboat
