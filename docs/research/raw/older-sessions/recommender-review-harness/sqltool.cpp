// sqltool <out.txt> <sql> <shard.sqlite>...   read-only, immutable; appends "code\tcol\tcol..." lines
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlError>
#include <QTextStream>
#include <QVariant>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    QFile out(args.at(1));
    out.open(QIODevice::WriteOnly | QIODevice::Append);
    QTextStream ts(&out);
    ts.setEncoding(QStringConverter::Utf8);
    const QString sql = args.at(2);
    for (int i = 3; i < args.size(); ++i) {
        const QString file = args.at(i);
        const QString code = QFileInfo(file).completeBaseName();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", code);
            db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI;QSQLITE_BUSY_TIMEOUT=0");
            QString path = QDir::fromNativeSeparators(file);
            if (!path.startsWith('/')) path.prepend('/');
            db.setDatabaseName("file://" + path + "?immutable=1");
            if (!db.open()) { ts << code << "\tOPEN FAIL\n"; continue; }
            QSqlQuery q(db);
            if (!q.exec(sql)) { ts << code << "\tERR " << q.lastError().text() << "\n"; }
            while (q.next()) {
                ts << code;
                for (int c = 0; c < q.record().count(); ++c) ts << '\t' << q.value(c).toString();
                ts << '\n';
            }
        }
        QSqlDatabase::removeDatabase(code);
    }
    return 0;
}
