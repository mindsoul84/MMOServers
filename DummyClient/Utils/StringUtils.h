#pragma once
#include <string>

// 윈도우 콘솔 문자열(CP949)을 UTF-8로 변환하는 함수
std::string AnsiToUtf8(const std::string& ansiStr);

// 지정된 길이의 영문 대문자+숫자 조합 난수 생성
std::string GenerateRandomID(int length);

// 지정된 길이의 영문 대소문자+특수문자 조합 난수 생성
std::string GenerateRandomPW(int length);