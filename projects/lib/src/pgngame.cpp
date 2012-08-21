/*
    This file is part of Cute Chess.

    Cute Chess is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Cute Chess is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Cute Chess.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pgngame.h"
#include <QStringList>
#include <QFile>
#include "board/boardfactory.h"
#include "econode.h"
#include "pgnstream.h"

PgnStream& operator>>(PgnStream& in, PgnGame& game)
{
	game.read(in);
	return in;
}

QTextStream& operator<<(QTextStream& out, const PgnGame& game)
{
	game.write(out);
	return out;
}


PgnGame::PgnGame()
	: m_startingSide(Chess::Side::White),
	  m_eco(EcoNode::root())
{
}

bool PgnGame::isNull() const
{
	return (m_tags.isEmpty() && m_moves.isEmpty());
}

void PgnGame::clear()
{
	m_startingSide = Chess::Side();
	m_eco = EcoNode::root();
	m_tags.clear();
	m_moves.clear();
}

const QVector<PgnGame::MoveData>& PgnGame::moves() const
{
	return m_moves;
}

void PgnGame::addMove(const MoveData& data)
{
	m_moves.append(data);

	m_eco = (m_eco && isStandard()) ? m_eco->child(data.moveString) : 0;
	if (m_eco && m_eco->isLeaf())
	{
		setTag("ECO", m_eco->ecoCode());
		setTag("Opening", m_eco->opening());
		setTag("Variation", m_eco->variation());
	}
}

Chess::Board* PgnGame::createBoard() const
{
	Chess::Board* board = Chess::BoardFactory::create(variant());
	if (board == 0)
		return 0;

	bool ok = true;

	QString fen(startingFenString());
	if (!fen.isEmpty())
		ok = board->setFenString(fen);
	else
	{
		board->reset();
		ok = !board->isRandomVariant();
	}
	if (!ok)
	{
		delete board;
		return 0;
	}

	return board;
}

bool PgnGame::parseMove(PgnStream& in)
{
	if (m_tags.isEmpty())
	{
		qDebug("No tags found");
		return false;
	}

	Chess::Board* board(in.board());

	// If the FEN string wasn't already set by the FEN tag,
	// set the board when we get the first move
	if (m_moves.isEmpty())
	{
		QString tmp(m_tags.value("Variant"));

		if (!tmp.isEmpty() && !in.setVariant(tmp))
		{
			qDebug("Unknown variant: %s", qPrintable(tmp));
			return false;
		}
		board = in.board();
		if (tmp.isEmpty() && board->variant() != "standard")
			m_tags["Variant"] = board->variant();

		tmp = m_tags.value("FEN");
		if (tmp.isEmpty())
		{
			if (board->isRandomVariant())
			{
				qDebug("Missing FEN tag");
				return false;
			}
			tmp = board->defaultFenString();
		}

		if (!board->setFenString(tmp))
		{
			qDebug("Invalid FEN string: %s", qPrintable(tmp));
			return false;
		}
		m_startingSide = board->startingSide();
	}

	const QString str(in.tokenString());
	Chess::Move move(board->moveFromString(str));
	if (move.isNull())
	{
		qDebug("Illegal move: %s", qPrintable(str));
		return false;
	}

	MoveData md = { board->key(), board->genericMove(move),
			str, QString() };
	addMove(md);

	board->makeMove(move);
	return true;
}

bool PgnGame::read(PgnStream& in, int maxMoves)
{
	clear();
	if (!in.nextGame())
		return false;
	
	while (in.status() == PgnStream::Ok)
	{
		bool stop = false;

		switch (in.readNext())
		{
		case PgnStream::PgnTag:
			m_tags[in.tagName()] = in.tagValue();
			break;
		case PgnStream::PgnMove:
			stop = !parseMove(in) || m_moves.size() >= maxMoves;
			break;
		case PgnStream::PgnComment:
			if (!m_moves.isEmpty())
				m_moves.last().comment.append(in.tokenString());
			break;
		case PgnStream::PgnResult:
			{
				const QString str(in.tokenString());
				QString& tag = m_tags["Result"];

				if (!tag.isEmpty() && str != tag)
					qDebug("%s",qPrintable(QString("Line %1: The termination "
						"marker is different from the result tag").arg(in.lineNumber())));
				tag = str;
			}
			stop = true;
			break;
		case PgnStream::PgnNag:
			{
				bool ok;
				int nag = in.tokenString().toInt(&ok);
				if (!ok || nag < 0 || nag > 255)
					qDebug("Invalid NAG: %s", qPrintable(in.tokenString()));
			}
			break;
		case PgnStream::NoToken:
			stop = true;
			break;
		default:
			break;
		}

		if (stop)
			break;
	}
	if (m_tags.isEmpty())
		return false;

	m_tags["PlyCount"] = QString::number(m_moves.size());

	return true;
}

static void writeTag(QTextStream& out, const QString& tag, const QString& value)
{
	if (!value.isEmpty())
		out << "[" << tag << " \"" << value << "\"]\n";
	else
		out << "[" << tag << " \"?\"]\n";
}

void PgnGame::write(QTextStream& out, PgnMode mode) const
{
	if (m_tags.isEmpty())
		return;
	
	// The seven tag roster
	QStringList roster;
	roster << "Event" << "Site" << "Date" << "Round"
	       << "White" << "Black" << "Result";
	foreach (const QString& tag, roster)
		writeTag(out, tag, m_tags.value(tag));
	
	// Other supported tags
	if (mode == Verbose)
	{
		QMap<QString, QString>::const_iterator it;
		for (it = m_tags.constBegin(); it != m_tags.constEnd(); ++it)
		{
			if (!roster.contains(it.key()) && !it.value().isEmpty())
				writeTag(out, it.key(), it.value());
		}
	}
	else if (mode == Minimal && m_tags.contains("FEN"))
	{
		writeTag(out, "FEN", m_tags["FEN"]);
		writeTag(out, "SetUp", m_tags["SetUp"]);
	}

	QString str;
	int lineLength = 0;
	int movenum = 0;
	int side = m_startingSide;

	for (int i = 0; i < m_moves.size(); i++)
	{
		const MoveData& data = m_moves.at(i);

		str.clear();
		if (side == Chess::Side::White || i == 0)
			str = QString::number(++movenum) + ". ";

		str += data.moveString;
		if (mode == Verbose && !data.comment.isEmpty())
			str += QString(" {%1}").arg(data.comment);

		// Limit the lines to 80 characters
		if (lineLength == 0 || lineLength + str.size() >= 80)
		{
			out << "\n" << str;
			lineLength = str.size();
		}
		else
		{
			out << " " << str;
			lineLength += str.size() + 1;
		}

		side = !side;
	}
	str = m_tags.value("Result");
	if (lineLength + str.size() >= 80)
		out << "\n" << str << "\n\n";
	else
		out << " " << str << "\n\n";
}

bool PgnGame::write(const QString& filename, PgnMode mode) const
{
	if (m_tags.isEmpty())
		return false;

	QFile file(filename);
	if (!file.open(QIODevice::Append))
		return false;

	QTextStream out(&file);
	write(out, mode);
	return true;
}

bool PgnGame::isStandard() const
{
	return variant() == "standard" && !m_tags.contains("FEN");
}

QString PgnGame::tagValue(const QString& tag) const
{
	return m_tags.value(tag);
}

QString PgnGame::event() const
{
	return m_tags.value("Event");
}

QString PgnGame::site() const
{
	return m_tags.value("Site");
}

QDate PgnGame::date() const
{
	return QDate::fromString(m_tags.value("Date"), "yyyy.MM.dd");
}

int PgnGame::round() const
{
	return m_tags.value("Round").toInt();
}

QString PgnGame::playerName(Chess::Side side) const
{
	if (side == Chess::Side::White)
		return m_tags.value("White");
	else if (side == Chess::Side::Black)
		return m_tags.value("Black");

	return QString();
}

Chess::Result PgnGame::result() const
{
	return Chess::Result(m_tags.value("Result"));
}

QString PgnGame::variant() const
{
	if (m_tags.contains("Variant"))
		return m_tags.value("Variant");
	return "standard";
}

Chess::Side PgnGame::startingSide() const
{
	return m_startingSide;
}

QString PgnGame::startingFenString() const
{
	return m_tags.value("FEN");
}

void PgnGame::setTag(const QString& tag, const QString& value)
{
	if (value.isEmpty())
		m_tags.remove(tag);
	else
		m_tags[tag] = value;
}

void PgnGame::setEvent(const QString& event)
{
	m_tags["Event"] = event;
}

void PgnGame::setSite(const QString& site)
{
	m_tags["Site"] = site;
}

void PgnGame::setDate(const QDate& date)
{
	m_tags["Date"] = date.toString("yyyy.MM.dd");
}

void PgnGame::setRound(int round)
{
	m_tags["Round"] = QString::number(round);
}

void PgnGame::setPlayerName(Chess::Side side, const QString& name)
{
	if (side == Chess::Side::White)
		m_tags["White"] = name;
	else if (side == Chess::Side::Black)
		m_tags["Black"] = name;
}

void PgnGame::setResult(const Chess::Result& result)
{
	m_tags["Result"] = result.toShortString();

	switch (result.type())
	{
	case Chess::Result::Adjudication:
		m_tags["Termination"] = "adjudication";
		break;
	case Chess::Result::Timeout:
		m_tags["Termination"] = "time forfeit";
		break;
	case Chess::Result::Disconnection:
		m_tags["Termination"] = "abandoned";
		break;
	case Chess::Result::NoResult:
		m_tags["Termination"] = "unterminated";
		break;
	default:
		m_tags.remove("Termination");
		break;
	}
}

void PgnGame::setVariant(const QString& variant)
{
	if (variant == "standard")
		m_tags.remove("Variant");
	else
		m_tags["Variant"] = variant;
}

void PgnGame::setStartingSide(Chess::Side side)
{
	m_startingSide = side;
}

void PgnGame::setStartingFenString(Chess::Side side, const QString& fen)
{
	m_startingSide = side;
	if (fen.isEmpty())
	{
		m_tags.remove("FEN");
		m_tags.remove("SetUp");
	}
	else
	{
		m_tags["FEN"] = fen;
		m_tags["SetUp"] = "1";
	}
}

void PgnGame::setResultDescription(const QString& description)
{
	if (description.isEmpty() || m_moves.isEmpty())
		return;

	QString& comment = m_moves.last().comment;
	if (!comment.isEmpty())
		comment += ", ";
	comment += description;
}
