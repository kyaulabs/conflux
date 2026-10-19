// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/StemServiceJsonl.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <QtTest>
#include <stdexcept>

using conflux::library::detail::StemServiceJsonl;

namespace {
QByteArray line(QByteArray body)
{
    body += '\n';
    return body;
}

QJsonObject one(const QByteArray& bytes)
{
    StemServiceJsonl parser;
    auto result = parser.feed(bytes);
    if (result.consumed != bytes.size() || !result.object)
        throw std::logic_error("One complete service line did not return one object");
    parser.finish();
    return std::move(*result.object);
}

void invalid(const QByteArray& bytes)
{
    StemServiceJsonl parser;
    QVERIFY_EXCEPTION_THROWN(parser.feed(bytes), std::invalid_argument);
    QVERIFY_EXCEPTION_THROWN(parser.feed(QByteArrayView{}), std::logic_error);
    QVERIFY_EXCEPTION_THROWN(parser.finish(), std::logic_error);
}

void limited(const QByteArray& bytes)
{
    StemServiceJsonl parser;
    QVERIFY_EXCEPTION_THROWN(parser.feed(bytes), std::length_error);
    QVERIFY_EXCEPTION_THROWN(parser.feed(QByteArrayView{}), std::logic_error);
    QVERIFY_EXCEPTION_THROWN(parser.finish(), std::logic_error);
}

QByteArray members(std::size_t count)
{
    QByteArray object("{");
    for (std::size_t index = 0; index < count; ++index) {
        if (index) object += ',';
        object += "\"k" + QByteArray::number(qulonglong(index)) + "\":0";
    }
    return object + '}';
}

QByteArray array(std::size_t count, const QByteArray& element = "0")
{
    QByteArray result("[");
    for (std::size_t index = 0; index < count; ++index) {
        if (index) result += ',';
        result += element;
    }
    return result + ']';
}

QByteArray framedBodyOfSize(qsizetype size)
{
    const QByteArray prefix("{\"message\":\""), suffix("\"}");
    if (size < prefix.size() + suffix.size())
        throw std::logic_error("Invalid fixture body size");
    return line(prefix + QByteArray(size - prefix.size() - suffix.size(), 'x') + suffix);
}
} // namespace

class StemServiceJsonlTest : public QObject {
    Q_OBJECT
private slots:
    void partialAndMultipleLines()
    {
        StemServiceJsonl parser;
        const auto prefix = QByteArray("{\"id\":");
        auto first = parser.feed(prefix);
        QCOMPARE(first.consumed, prefix.size());
        QVERIFY(!first.object);
        const QByteArray remainder("7}\n{\"message\":\"next\"}\n");
        auto completed = parser.feed(remainder);
        QCOMPARE(completed.consumed, qsizetype(3));
        QVERIFY(completed.object);
        QCOMPARE(completed.object->value("id").toInteger(), qint64(7));
        auto next = parser.feed(QByteArrayView(remainder).sliced(completed.consumed));
        QCOMPARE(next.consumed, remainder.size() - completed.consumed);
        QVERIFY(next.object);
        QCOMPARE(next.object->value("message").toString(), QString("next"));
        parser.finish();
        parser.finish();
        auto empty = parser.feed(QByteArrayView{});
        QCOMPARE(empty.consumed, qsizetype(0));
        QVERIFY(!empty.object);
        QVERIFY_EXCEPTION_THROWN(parser.feed(QByteArrayView("{}\n")), std::logic_error);

        // A later malformed suffix must not invalidate an earlier complete line.
        StemServiceJsonl separate;
        const QByteArray combined("{\"message\":\"safe\"}\nnot-json\n");
        auto safe = separate.feed(combined);
        QVERIFY(safe.object);
        QCOMPARE(safe.object->value("message").toString(), QString("safe"));
        QVERIFY_EXCEPTION_THROWN(
            separate.feed(QByteArrayView(combined).sliced(safe.consumed)), std::invalid_argument);

        // The per-line cap does not apply to an unconsumed suffix in the same
        // caller-owned view; do not scan/copy it before returning this first LF.
        StemServiceJsonl largeSuffix;
        const QByteArray shortLine("{\"message\":\"first\"}\n");
        const QByteArray suffix(StemServiceJsonl::maxLineBytes + 1, 'x');
        const QByteArray both = shortLine + suffix;
        const auto bounded = largeSuffix.feed(both);
        QCOMPARE(bounded.consumed, shortLine.size());
        QVERIFY(bounded.object);
        QCOMPARE(bounded.object->value("message").toString(), QString("first"));
        QVERIFY_EXCEPTION_THROWN(
            largeSuffix.feed(QByteArrayView(both).sliced(bounded.consumed)), std::length_error);
    }

    void fragmentedUnicodeAndEof()
    {
        QByteArray bytes("{\"message\":\"caf");
        bytes += QByteArray::fromHex("c3a9f09f9883");
        bytes += "\"}\n";
        StemServiceJsonl parser;
        std::optional<QJsonObject> object;
        for (qsizetype index = 0; index < bytes.size(); ++index) {
            const auto result = parser.feed(QByteArrayView(bytes).sliced(index, 1));
            QCOMPARE(result.consumed, qsizetype(1));
            QCOMPARE(result.object.has_value(), index == bytes.size() - 1);
            if (result.object) object = result.object;
        }
        QVERIFY(object);
        QCOMPARE(object->value("message").toString(), QString::fromUtf8("caf\xc3\xa9\xf0\x9f\x98\x83"));
        parser.finish();

        StemServiceJsonl empty;
        empty.finish();
        empty.finish();
        for (const QByteArray partial : {QByteArray("{\"id\":7}"), QByteArray(" "), QByteArray("{\"x\":\"")}) {
            StemServiceJsonl truncated;
            const auto result = truncated.feed(partial);
            QCOMPARE(result.consumed, partial.size());
            QVERIFY(!result.object);
            QVERIFY_EXCEPTION_THROWN(truncated.finish(), std::invalid_argument);
            QVERIFY_EXCEPTION_THROWN(truncated.feed(QByteArrayView{}), std::logic_error);
        }
        QCOMPARE(one("{\"message\":\"CRLF\"}\r\n").value("message").toString(), QString("CRLF"));
    }

    void duplicateDecodedKeys()
    {
        invalid(line(R"({"event":"bad","event":"chunk"})"));
        invalid(line(R"({"event":"bad","\u0065vent":"chunk"})"));
        invalid(line(R"({"source_id":"bad","\u0073ource_id":"good"})"));
        invalid(line(R"({"describe":{"input":{"sample_rate":48000,"sample_\u0072ate":44100}}})"));
        invalid(line(R"({"a/b":1,"a\/b":2})"));
        invalid(line("{\"a\\\"b\":1,\"a\\u0022b\":2}"));
        invalid(line(R"({"a\\b":1,"a\u005cb":2})"));
        invalid(line(R"({"payload":[{"x":1,"x":2}]})"));

        QByteArray sameUnicode("{\"");
        sameUnicode += QByteArray::fromHex("f09f9883");
        sameUnicode += "\":1,\"\\ud83d\\ude03\":2}";
        invalid(line(sameUnicode));

        const auto repeated = one(line(
            R"({"source":{"sample_rate":44100},"describe":{"input":{"sample_rate":44100}}})"));
        QCOMPARE(repeated.size(), 2);
        const auto cases = one(line(R"({"event":"one","Event":"two"})"));
        QCOMPARE(cases.size(), 2);
        const auto distinct = one(line(R"({"\u00e9":1,"e\u0301":2})"));
        QCOMPARE(distinct.size(), 2); // No NFC normalization.
        const auto text = one(line("{\"message\":\"{\\\"id\\\":1,\\\"id\\\":2} braces } ] and slash \\\\\"}"));
        QVERIFY(text.value("message").toString().contains("{\"id\":1,\"id\":2}"));
    }

    void unicodeAndJsonFailures()
    {
        for (const auto body : {
                R"({"message":"\ud800"})", R"({"message":"\udfff"})",
                R"({"message":"\ud800x"})", R"({"message":"\ud800\u0041"})",
                R"({"message":"\udc00\ud800"})", R"({"\ud800":1})",
                R"({"message":"\q"})", R"({"message":"\u00xx"})",
                R"({"id":1,})", R"({"id":01})", R"({"id":+1})",
                R"({"mean":NaN})", R"({"mean":Infinity})", R"({"mean":1e400})",
                R"({"x":1}{"x":2})", R"([])", R"(null)", R"(false)"})
            invalid(line(body));
        invalid("\n");
        invalid(QByteArray::fromHex("efbbbf") + "{}\n");
        for (const auto raw : {"c0af", "eda080", "f4908080", "e282"}) {
            QByteArray body("{\"message\":\"");
            body += QByteArray::fromHex(raw);
            body += "\"}";
            invalid(line(body));
        }
        QByteArray control("{\"message\":\"");
        control += '\0';
        control += "\"}";
        invalid(line(control));
        invalid(line(R"({"\u0000":1})"));

        const auto valid = one(line(R"({"message":"\ud83d\ude03 \udbff\udfff \u0000"})"));
        const auto message = valid.value("message").toString();
        QVERIFY(message.contains(QChar(0))); // Valid escaped NUL value; forbidden only in a key.
        QCOMPARE(message.size(), qsizetype(7));
    }

    void exactWireIntegerTokens()
    {
        const auto valid = one(line(
            R"({"v":0,"id":9007199254740991,"source":{"frames":-0},"mean":-2.75,"timestamp":1e-3})"));
        QCOMPARE(valid.value("id").toInteger(), qint64(9007199254740991LL));
        QCOMPARE(valid.value("source").toObject().value("frames").toInteger(), qint64(0));
        QCOMPARE(valid.value("mean").toDouble(), -2.75);
        QCOMPARE(valid.value("timestamp").toDouble(), 0.001);
        for (const auto number : {"1.0", "1e0", "-1", "9007199254740992",
                                  "9007199254740991.1", "18446744073709551616"}) {
            invalid(line(QByteArray("{\"id\":") + number + '}'));
            invalid(line(QByteArray(R"({"\u0069d":)") + number + '}'));
            invalid(line(QByteArray("{\"ready\":{\"bytes\":") + number + "}}"));
        }
        // Shape/parser success is not schema success: these are deliberately
        // accepted here; the later exact event schema must reject wrong types.
        const auto deferred = one(line(
            R"({"id":true,"generation":"1","sequence":null,"slot":{},"frames":[]})"));
        QVERIFY(deferred.value("id").isBool());
        QVERIFY(deferred.value("generation").isString());
        QVERIFY(deferred.value("sequence").isNull());
        QVERIFY(deferred.value("slot").isObject());
        QVERIFY(deferred.value("frames").isArray());
        QCOMPARE(one(line(R"({"mean":1e20})")).value("mean").toDouble(), 1e20);
    }

    void lineAndKeyBounds()
    {
        const auto exact = framedBodyOfSize(StemServiceJsonl::maxBodyBytes);
        QCOMPARE(exact.size(), StemServiceJsonl::maxLineBytes);
        QVERIFY(one(exact).value("message").isString());
        limited(framedBodyOfSize(StemServiceJsonl::maxBodyBytes + 1));
        limited(QByteArray(StemServiceJsonl::maxBodyBytes + 1, ' '));

        StemServiceJsonl waiting;
        const auto withoutLf = exact.first(exact.size() - 1);
        const auto pending = waiting.feed(withoutLf);
        QVERIFY(!pending.object);
        QCOMPARE(pending.consumed, withoutLf.size());
        QVERIFY_EXCEPTION_THROWN(waiting.finish(), std::invalid_argument);

        const QByteArray key(StemServiceJsonl::maxKeyUtf16Units, 'k');
        QVERIFY(one(line("{\"" + key + "\":0}")).contains(QString::fromLatin1(key)));
        limited(line("{\"" + key + "k\":0}"));
        QByteArray escapedKey;
        for (qsizetype index = 0; index < StemServiceJsonl::maxKeyUtf16Units; ++index)
            escapedKey += R"(\u006b)";
        QVERIFY(one(line("{\"" + escapedKey + "\":0}")).contains(QString::fromLatin1(key)));
        QByteArray emojiKey;
        const auto emoji = QByteArray::fromHex("f09f9883");
        for (qsizetype index = 0; index < StemServiceJsonl::maxKeyUtf16Units / 2; ++index)
            emojiKey += emoji;
        QVERIFY(one(line("{\"" + emojiKey + "\":0}")).contains(QString::fromUtf8(emojiKey)));
        limited(line("{\"" + emojiKey + emoji + "\":0}"));
        limited(line("{\"" + QByteArray(StemServiceJsonl::maxEncodedKeyBytes, 'x') + "\":0}"));

        // Number-token work is bounded even when Qt could decode a long finite
        // decimal. This is independent of the safe-integer member-name rule.
        const QByteArray number = "0." + QByteArray(StemServiceJsonl::maxNumberBytes - 3, '0') + '1';
        QCOMPARE(number.size(), StemServiceJsonl::maxNumberBytes);
        QVERIFY(one(line("{\"mean\":" + number + '}')).value("mean").isDouble());
        limited(line("{\"mean\":" + number + "0}"));
    }

    void structureBounds()
    {
        QByteArray nested("0");
        for (std::size_t level = 1; level < StemServiceJsonl::maxDepth; ++level)
            nested = '[' + nested + ']';
        QVERIFY(one(line("{\"x\":" + nested + '}')).value("x").isArray());
        limited(line("{\"x\":[" + nested + "]}"));
        QCOMPARE(one(line(members(StemServiceJsonl::maxObjectMembers))).size(),
                 qsizetype(StemServiceJsonl::maxObjectMembers));
        limited(line(members(StemServiceJsonl::maxObjectMembers + 1)));
        QCOMPARE(one(line("{\"x\":" + array(StemServiceJsonl::maxArrayElements) + '}'))
                     .value("x").toArray().size(), qsizetype(StemServiceJsonl::maxArrayElements));
        limited(line("{\"x\":" + array(StemServiceJsonl::maxArrayElements + 1) + '}'));

        // Four root members plus 32+32+32+28 nested members =128.
        const QByteArray common = "{\"a\":" + members(32) + ",\"b\":" + members(32) +
                                  ",\"c\":" + members(32) + ",\"d\":";
        QVERIFY(one(line(common + members(28) + '}')).value("d").isObject());
        limited(line(common + members(29) + '}'));

        // Root + outer array +15 child arrays +239 scalar elements =256.
        QByteArray values("{\"x\":[");
        for (std::size_t index = 0; index < 15; ++index) {
            if (index) values += ',';
            values += array(index == 14 ? 15 : 16);
        }
        values += "]}";
        QVERIFY(one(line(values)).value("x").isArray());
        // Add one scalar to the final15-element child, staying within each
        // array/member/depth cap but exceeding the total-value cap alone.
        values.insert(values.size() - 3, ",0");
        limited(line(values));
    }
};

QTEST_GUILESS_MAIN(StemServiceJsonlTest)
#include "stem_service_jsonl_test.moc"
