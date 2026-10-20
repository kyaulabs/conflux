// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/CueSheet.hpp"
#include "library/StemEngineSupport.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>
#include <QStringList>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdexcept>

namespace conflux::library {
namespace {
constexpr std::uint64_t maximumCueBytes = 1024U * 1024U;
constexpr qsizetype maximumLines = 8192, maximumLineLength = 4096;

[[noreturn]] void invalid(qsizetype line, const QString& reason)
{
    throw std::runtime_error(QString("CUE line %1: %2").arg(line).arg(reason).toStdString());
}

QStringList tokens(QStringView line, qsizetype number)
{
    QStringList result;
    qsizetype at = 0;
    while (at < line.size()) {
        while (at < line.size() && line[at].isSpace()) ++at;
        if (at == line.size()) break;
        const bool quoted = line[at] == QLatin1Char('"');
        if (quoted) ++at;
        const auto first = at;
        if (quoted) {
            while (at < line.size() && line[at] != QLatin1Char('"')) ++at;
            if (at == line.size()) invalid(number, "Unclosed quoted value");
        } else {
            while (at < line.size() && !line[at].isSpace()) {
                if (line[at] == QLatin1Char('"')) invalid(number, "Quote inside an unquoted value");
                ++at;
            }
        }
        result.push_back(line.mid(first, at - first).toString());
        if (quoted) {
            ++at;
            if (at < line.size() && !line[at].isSpace()) invalid(number, "Expected space after a quoted value");
        }
    }
    return result;
}

std::uint64_t decimal(QStringView value, qsizetype line)
{
    std::uint64_t result = 0;
    if (value.isEmpty()) invalid(line, "Missing decimal number");
    for (const auto digit : value) {
        if (digit < QLatin1Char('0') || digit > QLatin1Char('9')) invalid(line, "Expected unsigned decimal number");
        const auto next = std::uint64_t(digit.unicode() - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - next) / 10)
            invalid(line, "Decimal number overflow");
        result = result * 10 + next;
    }
    return result;
}

std::uint64_t cueFrame(const QString& value, qsizetype line)
{
    const auto parts = value.split(QLatin1Char(':'), Qt::KeepEmptyParts);
    if (parts.size() != 3) invalid(line, "Expected mm:ss:ff timestamp");
    const auto minutes = decimal(parts[0], line), seconds = decimal(parts[1], line), frames = decimal(parts[2], line);
    constexpr auto maximumTicks = std::numeric_limits<std::uint64_t>::max() / 640;
    if (seconds >= 60 || frames >= 75 || minutes > maximumTicks / 4500)
        invalid(line, "Timestamp is outside the 75-frame time format");
    const auto ticks = minutes * 4500 + seconds * 75 + frames;
    if (ticks > maximumTicks) invalid(line, "Timestamp frame overflow");
    return ticks * 640; // 48,000 / 75 is exact; no floating-point seek rounding.
}

std::vector<CuePoint> parse(const QString& text, const QString& cuePath,
                            const LibraryTrack& selected, std::stop_token stop)
{
    const auto lines = text.split(QLatin1Char('\n'));
    if (lines.size() > maximumLines) throw std::length_error("CUE line count exceeds limit");
    const auto selectedPath = QDir::cleanPath(selected.sourcePath);
    const QDir directory(QFileInfo(cuePath).absolutePath());
    std::vector<CuePoint> result;
    bool file = false, selectedFile = false, fileHasTrack = false, track = false, audio = false;
    int matchingFiles = 0, lastTrack = 0, trackNumber = 0, lastIndex = -1;
    QString title;
    bool titleSeen = false;
    std::optional<std::uint64_t> indexOne, lastIndexFrame, previousStart;
    auto finishTrack = [&](qsizetype line) {
        if (!track) return;
        if (!indexOne) invalid(line, "TRACK requires exactly one INDEX 01");
        if (previousStart && *indexOne <= *previousStart) invalid(line, "Track starts must increase within each FILE");
        previousStart = indexOne;
        if (selectedFile && audio) {
            if (*indexOne >= selected.frames) invalid(line, "Cue is outside the selected source");
            const auto label = title.trimmed().isEmpty() ? QString("Track %1").arg(trackNumber, 2, 10, QLatin1Char('0')) : title;
            result.push_back({trackNumber, label, *indexOne});
        }
        track = false;
    };
    for (qsizetype index = 0; index < lines.size(); ++index) {
        detail::checkStop(stop);
        const auto number = index + 1;
        if (lines[index].size() > maximumLineLength) invalid(number, "Line exceeds length limit");
        const auto line = lines[index].trimmed();
        if (line.isEmpty()) continue;
        const auto firstSpace = std::ranges::find_if(line, [](QChar c) { return c.isSpace(); });
        const auto command = line.first(std::distance(line.begin(), firstSpace)).toUpper();
        if (command == "REM") continue; // Comment payload is never interpreted.
        const auto fields = tokens(line, number);
        if (command == "FILE") {
            finishTrack(number);
            if (file && !fileHasTrack) invalid(number, "FILE has no TRACK");
            if (fields.size() != 3 || fields[1].isEmpty()) invalid(number, "Expected FILE path type");
            const auto type = fields[2].toUpper();
            if (type != "WAVE" && type != "AIFF" && type != "MP3" && type != "BINARY" && type != "MOTOROLA")
                invalid(number, "Unsupported FILE type");
            const auto path = QDir::cleanPath(QFileInfo(fields[1]).isAbsolute() ? fields[1] : directory.absoluteFilePath(fields[1]));
            selectedFile = path == selectedPath;
            if (selectedFile && (++matchingFiles > 1 || type == "BINARY" || type == "MOTOROLA"))
                invalid(number, "Selected source FILE is ambiguous or uses a raw data format");
            file = true; fileHasTrack = false; previousStart.reset();
        } else if (command == "TRACK") {
            finishTrack(number);
            if (!file || fields.size() != 3) invalid(number, "Expected TRACK number type after FILE");
            const auto value = decimal(fields[1], number);
            if (value < 1 || value > 99 || value <= std::uint64_t(lastTrack))
                invalid(number, "TRACK numbers must increase from 01 through 99");
            audio = fields[2].toUpper() == "AUDIO";
            if (selectedFile && !audio) invalid(number, "Selected FILE requires AUDIO tracks");
            trackNumber = lastTrack = int(value);
            track = fileHasTrack = true;
            title.clear(); titleSeen = false; indexOne.reset(); lastIndexFrame.reset(); lastIndex = -1;
        } else if (command == "INDEX") {
            if (!track || fields.size() != 3) invalid(number, "Expected INDEX number timestamp inside TRACK");
            const auto value = decimal(fields[1], number), frame = cueFrame(fields[2], number);
            if (value > 99 || int(value) <= lastIndex || (value > 1 && !indexOne) ||
                (lastIndexFrame && frame < *lastIndexFrame))
                invalid(number, "INDEX order or timestamp is invalid");
            if (value == 1) indexOne = frame;
            lastIndex = int(value); lastIndexFrame = frame;
        } else if (command == "TITLE") {
            if (fields.size() != 2) invalid(number, "Expected TITLE value (quote titles containing spaces)");
            if (track) {
                if (titleSeen || fields[1].size() > 256 || std::ranges::any_of(fields[1], [](QChar c) {
                        return c.category() == QChar::Other_Control; })) invalid(number, "Invalid or duplicate track TITLE");
                title = fields[1]; titleSeen = true;
            }
        } else if (command == "PREGAP" || command == "POSTGAP") {
            if (!track || fields.size() != 2) invalid(number, "Expected gap timestamp inside TRACK");
            static_cast<void>(cueFrame(fields[1], number)); // Virtual disc gaps do not change source offsets.
        } else if (command == "PERFORMER" || command == "SONGWRITER" || command == "CATALOG" ||
                   command == "CDTEXTFILE" || command == "FLAGS" || command == "ISRC") {
            if (fields.size() < 2) invalid(number, "Metadata command requires a value");
            // Authoring metadata is not a file-opening or playback instruction.
        } else {
            invalid(number, "Unsupported command " + command);
        }
    }
    finishTrack(lines.size());
    if (file && !fileHasTrack) invalid(lines.size(), "FILE has no TRACK");
    if (matchingFiles != 1 || result.empty())
        throw std::invalid_argument("CUE sheet has no AUDIO track starts for the selected source path");
    return result;
}
} // namespace

CueSheetResult readCueSheet(const QString& absoluteCuePath, const LibraryTrack& selected, std::stop_token stop)
{
    try {
        detail::checkStop(stop);
        if (!QFileInfo(absoluteCuePath).isAbsolute() || absoluteCuePath.contains(QChar(0)) ||
            !QFileInfo(selected.sourcePath).isAbsolute() || selected.sourcePath.contains(QChar(0)) ||
            !detail::hexDigest(selected.sourceSha256.toLatin1()) || !selected.frames ||
            selected.frames > audio::PreparedAudio::defaultMaxPreparedBytes / 8)
            throw std::invalid_argument("CUE import requires absolute local paths and a valid saved source");
        detail::Descriptor cue(open(QFile::encodeName(absoluteCuePath).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
        if (cue.get() < 0) detail::ioError("Cannot open CUE sheet");
        const auto before = detail::statusOf(cue.get());
        if (!S_ISREG(before.st_mode) || before.st_size < 1 || std::uint64_t(before.st_size) > maximumCueBytes)
            throw std::invalid_argument("CUE sheet must be a nonempty regular file of at most 1 MiB");
        QByteArray bytes(qsizetype(before.st_size), Qt::Uninitialized);
        for (qsizetype at = 0; at < bytes.size();) {
            detail::checkStop(stop);
            const auto count = pread(cue.get(), bytes.data() + at, std::size_t(std::min<qsizetype>(32768, bytes.size() - at)), at);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("CUE sheet changed or could not be read");
            at += count;
        }
        if (!detail::unchanged(before, detail::statusOf(cue.get()))) throw std::runtime_error("CUE sheet changed while reading");
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        QString text = decoder(bytes);
        if (decoder.hasError() || text.contains(QChar(0))) throw std::invalid_argument("CUE sheet must contain valid UTF-8 text without NUL");
        if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
        auto points = parse(text, absoluteCuePath, selected, stop);
        detail::Descriptor source(open(QFile::encodeName(selected.sourcePath).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (source.get() < 0) detail::ioError("Cannot open selected cue source");
        const auto sourceBefore = detail::statusOf(source.get());
        if (!S_ISREG(sourceBefore.st_mode) || sourceBefore.st_size < 0 ||
            std::uint64_t(sourceBefore.st_size) > PreparedTrackStore::Limits{}.maxSourceBytes)
            throw std::invalid_argument("Selected cue source must be a bounded regular file");
        detail::verifySource(source.get(), sourceBefore, selected.sourceSha256.toLatin1(), stop);
        struct stat current{};
        if (stat(QFile::encodeName(selected.sourcePath).constData(), &current) || !detail::unchanged(sourceBefore, current))
            throw std::runtime_error("Selected cue source changed during import");
        if (!detail::unchanged(before, detail::statusOf(cue.get()))) throw std::runtime_error("CUE sheet changed during import");
        struct stat currentCue{};
        if (lstat(QFile::encodeName(absoluteCuePath).constData(), &currentCue) ||
            !S_ISREG(currentCue.st_mode) || !detail::unchanged(before, currentCue))
            throw std::runtime_error("CUE sheet path changed during import");
        detail::checkStop(stop);
        return {std::move(points), {}, false};
    } catch (const std::exception& error) {
        return {{}, QString::fromUtf8(error.what()), stop.stop_requested()};
    }
}

} // namespace conflux::library
