#include "citius/util/AsciiText.hpp"
#include <gtest/gtest.h>
#include <string>

using citius::has_control_chars;
using citius::iequals;
using citius::to_lower_ascii;

TEST(AsciiTextTest, ToLowerAsciiConverteSoLetrasAscii) {
    EXPECT_EQ(to_lower_ascii('A'), 'a');
    EXPECT_EQ(to_lower_ascii('Z'), 'z');
    EXPECT_EQ(to_lower_ascii('a'), 'a');
    EXPECT_EQ(to_lower_ascii('5'), '5');
    EXPECT_EQ(to_lower_ascii('@'), '@');                                         // logo antes de 'A'
    EXPECT_EQ(to_lower_ascii('['), '[');                                         // logo depois de 'Z'
    EXPECT_EQ(to_lower_ascii(static_cast<char>(0xC4)), static_cast<char>(0xC4)); // nao ASCII: intacto
}

TEST(AsciiTextTest, ToLowerAsciiDeTexto) {
    EXPECT_EQ(to_lower_ascii("Content-LENGTH"), "content-length");
    EXPECT_EQ(to_lower_ascii(""), "");
}

TEST(AsciiTextTest, IequalsIgnoraMaiusculas) {
    EXPECT_TRUE(iequals("Keep-Alive", "keep-alive"));
    EXPECT_TRUE(iequals("", ""));
    EXPECT_FALSE(iequals("close", "closed"));
    EXPECT_FALSE(iequals("abc", "abd"));
}

TEST(AsciiTextTest, HasControlCharsAcusaCrLfNulEDel) {
    EXPECT_TRUE(has_control_chars("a\rb"));
    EXPECT_TRUE(has_control_chars("a\nb"));
    EXPECT_TRUE(has_control_chars(std::string("a\0b", 3)));
    EXPECT_TRUE(has_control_chars("a\x7f"));
    EXPECT_TRUE(has_control_chars("\x1b[31m")); // escape de terminal
}

TEST(AsciiTextTest, HasControlCharsAceitaTabETextoComum) {
    EXPECT_FALSE(has_control_chars("a\tb"));
    EXPECT_FALSE(has_control_chars("texto normal, com acentuacao: \xc3\xa7"));
    EXPECT_FALSE(has_control_chars(""));
}
