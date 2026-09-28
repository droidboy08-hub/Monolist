// probe <catalogue> <graph> <out.txt> <mode> [args...]
//  names <file>        : per name, catalogue match + credited rows, graph findArtist + neighbours
//  neighbours          : every artist that has an edge in any shard and passes creditedRows: shard, name, top rows + genre
//  catsearch <regex>   : catalogue rows whose artist or title matches (case-insensitive), max 400
//  catartist <regex>   : distinct catalogue artist names matching, with row count and top genre
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTextStream>
#include <QVariant>

#include "rec/catalog.h"
#include "rec/graph.h"
#include "rec/matchkey.h"

using namespace Rec;

static QString firstPerformer(const QString &artist)
{
    static const QRegularExpression separator(
        QStringLiteral(R"(\s*(?:,|&|;|/)\s*|\s+(?:feat\.?|ft\.?|with|x)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    return artist.section(separator, 0, 0).trimmed();
}
static QString plainName(const QString &name)
{
    QString out;
    for (const QChar c : name.normalized(QString::NormalizationForm_KD)) {
        if (c.category() == QChar::Mark_NonSpacing) continue;
        out += c.isLetterOrNumber() ? c.toLower() : QLatin1Char(' ');
    }
    return out.simplified();
}
static QVector<int> creditedRows(const Catalog &catalog, const QString &name, int limit)
{
    const Catalog::Match match = catalog.match(QString(), name);
    if (match.artistId < 0) return {};
    const QString wanted = plainName(name);
    if (wanted.isEmpty()) return {};
    QVector<int> rows;
    for (const int row : catalog.artistRows(match.artistId, 40)) {
        if (rows.size() >= limit) break;
        const QString credit = catalog.artist(row);
        if (plainName(credit) == wanted || plainName(firstPerformer(credit)) == wanted)
            rows.append(row);
    }
    return rows;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    Catalog catalog;
    Graph graph;
    catalog.load(args.at(1));
    graph.open(args.at(2));
    QFile outFile(args.at(3));
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    QTextStream out(&outFile);
    out.setEncoding(QStringConverter::Utf8);
    const QString mode = args.at(4);
    const auto row = [&](int r) {
        return catalog.artist(r) + " — " + catalog.title(r) + " [" + catalog.genreName(catalog.genreId(r)) + ", pop " + QString::number(catalog.popularity(r)) + "]";
    };

    if (mode == "names") {
        QFile f(args.at(5));
        if (!f.open(QIODevice::ReadOnly)) return 3;
        for (const QByteArray &line : f.readAll().split('\n')) {
            const QString name = QString::fromUtf8(line).trimmed();
            if (name.isEmpty() || name.startsWith('#')) continue;
            const Catalog::Match m = catalog.match(QString(), name);
            out << "\n### " << name << "   key \"" << primaryArtist(name) << "\"  firstPerformer \"" << firstPerformer(name) << "\"\n";
            out << "  catalogue lead for key: " << (m.artistId < 0 ? QStringLiteral("none") : catalog.artistName(m.artistId)) << "\n";
            if (m.artistId >= 0) {
                QStringList credits;
                QSet<QString> seen;
                for (int r : catalog.artistRows(m.artistId, 40)) {
                    if (!seen.contains(catalog.artist(r))) { seen.insert(catalog.artist(r)); credits << catalog.artist(r); }
                }
                out << "  credits in key group (" << credits.size() << "): " << credits.mid(0, 12).join(" | ") << "\n";
                const QVector<int> cr = creditedRows(catalog, name, 3);
                out << "  creditedRows(" << name << "): " << cr.size() << "\n";
                for (int r : cr) out << "     " << row(r) << "\n";
            }
            const GraphArtist g = graph.findArtist(name, "US");
            out << "  graph: " << (g.mbid.isEmpty() ? QStringLiteral("not found") : g.name + " / " + g.region) << "\n";
            if (!g.mbid.isEmpty()) {
                QStringList ns;
                for (const GraphNeighbour &n : graph.neighbours(g.mbid, g.region, 12)) {
                    const QString nm = graph.artistName(n.mbid, g.region);
                    ns << nm + (creditedRows(catalog, nm, 1).isEmpty() ? " -nocat" : "");
                }
                out << "  neighbours: " << ns.join(", ") << "\n";
            }
        }
    } else if (mode == "neighbours") {
        const QDir dir(args.at(2));
        QSet<QString> done;
        for (const QFileInfo &info : dir.entryInfoList({ "*.sqlite" }, QDir::Files)) {
            const QString code = info.completeBaseName();
            {
                QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "p_" + code);
                db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI;QSQLITE_BUSY_TIMEOUT=0");
                QString path = QDir::fromNativeSeparators(info.absoluteFilePath());
                if (!path.startsWith('/')) path.prepend('/');
                db.setDatabaseName("file://" + path + "?immutable=1");
                if (db.open()) {
                    QSqlQuery q(db);
                    q.exec("SELECT DISTINCT a.name, a.region, a.mbid FROM artists a WHERE a.mbid IN (SELECT a FROM edges UNION SELECT b FROM edges)");
                    while (q.next()) {
                        const QString name = q.value(0).toString();
                        const QString key = code + "|" + name;
                        if (done.contains(key)) continue;
                        done.insert(key);
                        const QVector<int> cr = creditedRows(catalog, name, 2);
                        if (cr.isEmpty()) continue;
                        QStringList flags;
                        for (int r : cr)
                            if (plainName(catalog.artist(r)) != plainName(name)) { flags << "VIAFP:" + catalog.artist(r); break; }
                        if (cr.size() > 1 && titleCore(catalog.title(cr.at(0))) == titleCore(catalog.title(cr.at(1))))
                            flags << "DUP";
                        out << (flags.isEmpty() ? QString("ok") : flags.join(",")) << "\t";
                        out << code << "\t" << q.value(1).toString() << "\t" << name << "\t" << row(cr.first())
                            << (cr.size() > 1 ? "\t| " + catalog.title(cr.at(1)) + " [" + catalog.genreName(catalog.genreId(cr.at(1))) + "]" : QString()) << "\n";
                    }
                }
            }
            QSqlDatabase::removeDatabase("p_" + code);
        }
    } else if (mode == "catsearch") {
        const QRegularExpression re(args.at(5), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
        int n = 0;
        for (int r = 0; r < catalog.count() && n < 400; ++r) {
            if (re.match(catalog.artist(r)).hasMatch() || re.match(catalog.title(r)).hasMatch()) {
                out << r << "\t" << row(r) << "\n";
                ++n;
            }
        }
        out << "matches: " << n << "\n";
    } else if (mode == "catgenre") {
        const QRegularExpression re(args.at(5), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
        QHash<QString, int> counts;
        QHash<QString, int> maxPop;
        for (int r = 0; r < catalog.count(); ++r) {
            const QString g = catalog.genreName(catalog.genreId(r));
            if (g.isEmpty() || !re.match(g).hasMatch()) continue;
            const QString key = catalog.artist(r) + " [" + g + "]";
            counts[key] += 1;
            maxPop[key] = std::max(maxPop.value(key), catalog.popularity(r));
        }
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
            out << it.key() << "\trows " << it.value() << "\tmaxpop " << maxPop.value(it.key()) << "\n";
    } else if (mode == "catartist") {
        const QRegularExpression re(args.at(5), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
        for (int id = 0; id < catalog.artistCount(); ++id) {
            const QString name = catalog.artistName(id);
            if (!re.match(name).hasMatch()) continue;
            out << id << "\t" << name << "\n";
        }
    }
    return 0;
}
