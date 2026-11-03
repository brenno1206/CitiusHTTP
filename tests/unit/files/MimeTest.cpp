#include "citius/files/Mime.hpp"
#include "citius/http/StatusCode.hpp"
#include <gtest/gtest.h>

using citius::mime_type;

TEST(MimeTest, ExtensoesConhecidas) {
    EXPECT_EQ(mime_type(".html"), "text/html; charset=utf-8");
    EXPECT_EQ(mime_type(".css"), "text/css; charset=utf-8");
    EXPECT_EQ(mime_type(".js"), "text/javascript; charset=utf-8");
    EXPECT_EQ(mime_type(".json"), "application/json");
    EXPECT_EQ(mime_type(".png"), "image/png");
    EXPECT_EQ(mime_type(".jpg"), "image/jpeg");
    EXPECT_EQ(mime_type(".jpeg"), "image/jpeg");
    EXPECT_EQ(mime_type(".svg"), "image/svg+xml");
    EXPECT_EQ(mime_type(".ico"), "image/x-icon");
    EXPECT_EQ(mime_type(".txt"), "text/plain; charset=utf-8");
}

TEST(MimeTest, IgnoraMaiusculas) {
    EXPECT_EQ(mime_type(".HTML"), "text/html; charset=utf-8");
    EXPECT_EQ(mime_type(".PnG"), "image/png");
}

TEST(MimeTest, DesconhecidaOuVaziaViraOctetStream) {
    EXPECT_EQ(mime_type(".xyz"), "application/octet-stream");
    EXPECT_EQ(mime_type(""), "application/octet-stream");
    EXPECT_EQ(mime_type("."), "application/octet-stream");
    EXPECT_EQ(mime_type("html"), "application/octet-stream"); // sem o ponto nao e extensao
}

TEST(StatusCodeTest, Forbidden) {
    EXPECT_EQ(static_cast<int>(citius::StatusCode::FORBIDDEN), 403);
    EXPECT_EQ(citius::reason_phrase(citius::StatusCode::FORBIDDEN), "Forbidden");
}
