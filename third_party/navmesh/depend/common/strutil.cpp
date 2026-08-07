/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2016 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "strutil.h"
#include <algorithm>
#include <limits>
#include <algorithm>
#include <utility>
#include <functional>
#include <cctype>

#include "memorystream.h"

namespace KBEngine{ 
namespace strutil {

	int bytes2string(unsigned char *src, int srcsize, unsigned char *dst, int dstsize)     
	{     
		if (dst != NULL)  
		{  
			*dst = 0;  
		}  
	      
		if (src == NULL || srcsize <= 0 || dst == NULL || dstsize <= srcsize * 2)  
		{  
			return 0;  
		}  
	      
		const char szTable[] = "0123456789ABCDEF";

		for(int i=0; i<srcsize; ++i)     
		{     
			*dst++ = szTable[src[i] >> 4];     
			*dst++ = szTable[src[i] & 0x0f];   
		}     
 
		*dst = 0;      
		return  srcsize * 2;     
	}

	int string2bytes(unsigned char* src, unsigned char* dst, int dstsize)     
	{  
		if(src == NULL)
			return 0;  

		int iLen = strlen((char *)src);  
		if (iLen <= 0 || iLen%2 != 0 || dst == NULL || dstsize < iLen/2)  
		{  
			return 0;  
		}  
	      
		iLen /= 2;  
		str_toupper((char *)src); 
		for (int i=0; i<iLen; ++i)  
		{  
			int iVal = 0;  
			unsigned char *pSrcTemp = src + i*2;  
			sscanf((char *)pSrcTemp, "%02x", &iVal);  
			dst[i] = (unsigned char)iVal;  
		}  
	      
		return iLen;  
	}

    std::string toLower(const std::string& str) {
        std::string t = str;
        std::transform(t.begin(), t.end(), t.begin(), tolower);
        return t;
    }

    std::string toUpper(const std::string& str) {
        std::string t = str;
        std::transform(t.begin(), t.end(), t.begin(), toupper);
        return t;
    }

	std::string &kbe_ltrim(std::string &s) 
	{
		s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) { return !std::isspace(ch); }));
		return s;
	}

	std::string &kbe_rtrim(std::string &s) 
	{
		s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) { return !std::isspace(ch); }).base(), s.end());
		return s;
	}


	std::string kbe_trim(std::string s) 
	{
		return kbe_ltrim(kbe_rtrim(s));
	}

	// 字符串替换
	int kbe_replace(std::string& str,  const std::string& pattern,  const std::string& newpat) 
	{ 
		int count = 0; 
		const size_t nsize = newpat.size(); 
		const size_t psize = pattern.size(); 

		for(size_t pos = str.find(pattern, 0);  
			pos != std::string::npos; 
			pos = str.find(pattern,pos + nsize)) 
		{ 
			str.replace(pos, psize, newpat); 
			count++; 
		} 

		return count; 
	}

	int kbe_replace(std::wstring& str,  const std::wstring& pattern,  const std::wstring& newpat) 
	{ 
		int count = 0; 
		const size_t nsize = newpat.size(); 
		const size_t psize = pattern.size(); 

		for(size_t pos = str.find(pattern, 0);  
			pos != std::wstring::npos; 
			pos = str.find(pattern,pos + nsize)) 
		{ 
			str.replace(pos, psize, newpat); 
			count++; 
		} 

		return count; 
	}


	std::vector< std::string > kbe_splits(const std::string& s, const std::string& delim, const bool keep_empty) 
	{
		std::vector< std::string > result;

		if (delim.empty()) {
			result.push_back(s);
			return result;
		}

		std::string::const_iterator substart = s.begin(), subend;

		while (true) {
			subend = std::search(substart, s.end(), delim.begin(), delim.end());
			std::string temp(substart, subend);
			if (keep_empty || !temp.empty()) {
				result.push_back(temp);
			}
			if (subend == s.end()) {
				break;
			}
			substart = subend + delim.size();
		}

		return result;
	}

	char* wchar2char(const wchar_t* ts, size_t* outlen)
	{
		int len = (wcslen(ts) + 1) * 4/* Linux下sizeof(wchar_t) == 4, Windows下是2字节，这里取大的 */;
		char* ccattr =(char *)malloc(len);
		memset(ccattr, 0, len);

		size_t slen = wcstombs(ccattr, ts, len);

		if(outlen)
			*outlen = slen;

		return ccattr;
	};

	void wchar2char(const wchar_t* ts, MemoryStream* pStream)
	{
		int len = (wcslen(ts) + 1) * 4/* Linux下sizeof(wchar_t) == 4, Windows下是2字节，这里取大的 */;
		pStream->data_resize(pStream->wpos() + len);
		size_t slen = wcstombs((char*)&pStream->data()[pStream->wpos()], ts, len);
		pStream->wpos(pStream->wpos() + slen + 1);
	};

	wchar_t* char2wchar(const char* cs, size_t* outlen)
	{
		int len = (strlen(cs) + 1) * 4/* Linux下sizeof(wchar_t) == 4, Windows下是2字节，这里取大的 */;
		wchar_t* ccattr =(wchar_t *)malloc(len);
		memset(ccattr, 0, len);

		size_t slen = mbstowcs(ccattr, cs, len);

		if(outlen)
			*outlen = slen;

		return ccattr;
	};

	/*
	int wchar2utf8(const wchar_t* in, int in_len, char* out, int out_max)   
	{   
	#if KBE_PLATFORM == PLATFORM_WIN32   
		BOOL use_def_char;   
		use_def_char = FALSE;   
		return ::WideCharToMultiByte(CP_UTF8, 0, in,in_len / sizeof(wchar_t), out, out_max, NULL, NULL);   
	#else   
		size_t result;   
		iconv_t env;   
	   
		env = iconv_open("UTF8", "WCHAR_T");   
		result = iconv(env,(char**)&in,(size_t*)&in_len,(char**)&out,(size_t*)&out_max);        
		iconv_close(env);   
		return (int) result;   
	#endif   
	}   
	   
	int wchar2utf8(const std::wstring& in, std::string& out)   
	{   
		int len = in.length() + 1;   
		int result;   

		char* pBuffer = new char[len * 4];   

		memset(pBuffer,0,len * 4);               

		result = wchar2utf8(in.c_str(), in.length() * sizeof(wchar_t), pBuffer,len * 4);   

		if(result >= 0)   
		{   
			out = pBuffer;   
		}   
		else   
		{   
			out = "";   
		}   

		delete[] pBuffer;   
		return result;   
	}   
	   
	int utf82wchar(const char* in, int in_len, wchar_t* out, int out_max)   
	{   
	#if KBE_PLATFORM == PLATFORM_WIN32   
		return ::MultiByteToWideChar(CP_UTF8, 0, in, in_len, out, out_max);   
	#else   
		size_t result;   
		iconv_t env;   
		env = iconv_open("WCHAR_T", "UTF8");   
		result = iconv(env,(char**)&in, (size_t*)&in_len, (char**)&out,(size_t*)&out_max);   
		iconv_close(env);   
		return (int) result;   
	#endif   
	}   
	   
	int utf82wchar(const std::string& in, std::wstring& out)   
	{   
		int len = in.length() + 1;   
		int result;   
	 
		wchar_t* pBuffer = new wchar_t[len];   
		memset(pBuffer,0,len * sizeof(wchar_t));   
		result = utf82wchar(in.c_str(), in.length(), pBuffer, len*sizeof(wchar_t));   

		if(result >= 0)   
		{   
			out = pBuffer;   
		}   
		else   
		{   
			out.clear();         
		}   

		delete[] pBuffer;   
		return result;   
	}   
	*/

	namespace
	{
		bool decodeNextUtf8(const char*& iter, const char* end, unsigned int& codepoint)
		{
			if (iter >= end)
				return false;

			unsigned char c0 = static_cast<unsigned char>(*iter++);
			if (c0 < 0x80)
			{
				codepoint = c0;
				return true;
			}

			unsigned int value = 0;
			int needed = 0;
			unsigned int minValue = 0;

			if ((c0 & 0xE0) == 0xC0)
			{
				value = c0 & 0x1F;
				needed = 1;
				minValue = 0x80;
			}
			else if ((c0 & 0xF0) == 0xE0)
			{
				value = c0 & 0x0F;
				needed = 2;
				minValue = 0x800;
			}
			else if ((c0 & 0xF8) == 0xF0)
			{
				value = c0 & 0x07;
				needed = 3;
				minValue = 0x10000;
			}
			else
			{
				return false;
			}

			if (end - iter < needed)
				return false;

			for (int i = 0; i < needed; ++i)
			{
				unsigned char cx = static_cast<unsigned char>(*iter++);
				if ((cx & 0xC0) != 0x80)
					return false;
				value = (value << 6) | (cx & 0x3F);
			}

			if (value < minValue || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
				return false;

			codepoint = value;
			return true;
		}

		void appendUtf8Codepoint(unsigned int codepoint, std::string& out)
		{
			if (codepoint < 0x80)
			{
				out.push_back(static_cast<char>(codepoint));
			}
			else if (codepoint < 0x800)
			{
				out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else if (codepoint < 0x10000)
			{
				out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
		}

		bool appendWideCodepoint(unsigned int codepoint, std::wstring& out)
		{
			if (sizeof(wchar_t) == 2 && codepoint > 0xFFFF)
			{
				codepoint -= 0x10000;
				out.push_back(static_cast<wchar_t>(0xD800 + (codepoint >> 10)));
				out.push_back(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
				return true;
			}

			if (codepoint > 0x10FFFF)
				return false;

			out.push_back(static_cast<wchar_t>(codepoint));
			return true;
		}

		bool utf8ToWide(const char* utf8str, size_t csize, std::wstring& wstr)
		{
			wstr.clear();
			const char* iter = utf8str;
			const char* end = utf8str + csize;

			while (iter < end)
			{
				unsigned int codepoint = 0;
				if (!decodeNextUtf8(iter, end, codepoint) || !appendWideCodepoint(codepoint, wstr))
				{
					wstr.clear();
					return false;
				}
			}

			return true;
		}

		bool wideToUtf8(const wchar_t* wstr, size_t size, std::string& utf8str)
		{
			utf8str.clear();

			for (size_t i = 0; i < size; ++i)
			{
				unsigned int codepoint = static_cast<unsigned int>(wstr[i]);

				if (sizeof(wchar_t) == 2)
				{
					if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
					{
						if (i + 1 >= size)
						{
							utf8str.clear();
							return false;
						}

						unsigned int low = static_cast<unsigned int>(wstr[++i]);
						if (low < 0xDC00 || low > 0xDFFF)
						{
							utf8str.clear();
							return false;
						}

						codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low - 0xDC00));
					}
					else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
					{
						utf8str.clear();
						return false;
					}
				}

				if (codepoint > 0x10FFFF)
				{
					utf8str.clear();
					return false;
				}

				appendUtf8Codepoint(codepoint, utf8str);
			}

			return true;
		}
	}

	size_t utf8length(std::string& utf8str)
	{
		const char* iter = utf8str.c_str();
		const char* end = iter + utf8str.size();
		size_t length = 0;

		while (iter < end)
		{
			unsigned int codepoint = 0;
			if (!decodeNextUtf8(iter, end, codepoint))
			{
				utf8str = "";
				return 0;
			}
			++length;
		}

		return length;
	}

	void utf8truncate(std::string& utf8str, size_t len)
	{
		const char* iter = utf8str.c_str();
		const char* end = iter + utf8str.size();
		std::string truncated;
		size_t length = 0;

		while (iter < end)
		{
			unsigned int codepoint = 0;
			if (!decodeNextUtf8(iter, end, codepoint))
			{
				utf8str = "";
				return;
			}

			if (length++ >= len)
			{
				utf8str = truncated;
				return;
			}

			appendUtf8Codepoint(codepoint, truncated);
		}
	}

	bool utf82wchar(char const* utf8str, size_t csize, 
		wchar_t* wstr, size_t& wsize)
	{
		std::wstring converted;
		if (!utf8ToWide(utf8str, csize, converted))
		{
			if (wstr && wsize > 0)
				wstr[0] = L'\0';
			wsize = 0;
			return false;
		}

		if (converted.size() + 1 > wsize)
		{
			if (wstr && wsize > 0)
				wstr[0] = L'\0';
			wsize = 0;
			return false;
		}

		if (!converted.empty())
			memcpy(wstr, converted.c_str(), converted.size() * sizeof(wchar_t));
		wstr[converted.size()] = L'\0';
		wsize = converted.size();
		return true;
	}

	bool utf82wchar(const std::string& utf8str, std::wstring& wstr)
	{
		return utf8ToWide(utf8str.c_str(), utf8str.size(), wstr);
	}

	bool wchar2utf8(const wchar_t* wstr, size_t size, std::string& utf8str)
	{
		return wideToUtf8(wstr, size, utf8str);
	}

	bool wchar2utf8(const std::wstring& wstr, std::string& utf8str)
	{
		return wideToUtf8(wstr.c_str(), wstr.size(), utf8str);
	}
}

}
