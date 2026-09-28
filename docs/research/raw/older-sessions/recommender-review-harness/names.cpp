#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <cmath>
#include "rec/catalog.h"
#include "rec/matchkey.h"
using namespace Rec;
static float cosRows(const Catalog &c, int a, int b) {
    const float *x = c.vector(a); const float *y = c.vector(b); double d = 0;
    for (int i = 0; i < c.dims(); ++i) d += double(x[i]) * y[i]; return float(d);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    Catalog c; c.load(args.at(1));
    QFile f(args.at(2)); f.open(QIODevice::WriteOnly); QTextStream out(&f); out.setEncoding(QStringConverter::Utf8);
    for (int n = 3; n < args.size(); ++n) {
        const QString name = args.at(n);
        const Catalog::Match m = c.match(QString(), name);
        out << "=== " << name << " artistId " << m.artistId << "\n";
        if (m.artistId < 0) continue;
        const QVector<int> rows = c.artistRows(m.artistId, 40);
        for (int r : rows) {
            if (plainName(c.artist(r)) != plainName(name)) continue;
            out << "  " << r << "\t" << c.artist(r) << "\t" << c.title(r) << "\t" << c.genreName(c.genreId(r)) << "\tpop " << c.popularity(r)
                << "\tcos-to-first " << QString::number(cosRows(c, r, rows.first()), 'f', 3) << "\n";
        }
    }
    return 0;
}