//##########################################################################
//#                                                                        #
//#                CLOUDCOMPARE PLUGIN: PythonPlugin                       #
//#                                                                        #
//#  This program is free software; you can redistribute it and/or modify  #
//#  it under the terms of the GNU General Public License as published by  #
//#  the Free Software Foundation; version 2 of the License.               #
//#                                                                        #
//#  This program is distributed in the hope that it will be useful,       #
//#  but WITHOUT ANY WARRANTY; without even the implied warranty of        #
//#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         #
//#  GNU General Public License for more details.                          #
//#                                                                        #
//#                   COPYRIGHT: Thomas Montaigu                           #
//#                                                                        #
//##########################################################################

#include "QPythonRepl.h"
#include "PythonHighlighter.h"
#include "PythonInterpreter.h"
#include <ccGuiPythonInstance.h>

#include <ui_QPythonREPL.h>

#include "PythonStdErrOutRedirect.h"

namespace py = pybind11;

/// REPL special starting characters
/// The first line of the REPL codeEdit shall start with the arrows
/// subsequent lines shall start with the dots
const static QString replArrows(QStringLiteral(">>> "));
const static QString continuationDots(QStringLiteral("... "));

/// Handles key presses on the REPL window.
///
/// It does a few things:
///    - maps up/down arrow keys to history navigation
///    - blocks movements/deletions/edits of the cursor when it would erase
///      the REPL special characters
bool KeyPressEater::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->modifiers() & Qt::ShiftModifier &&
            (keyEvent->key() == Qt::Key_Enter || keyEvent->key() == Qt::Key_Return))
        {
            m_repl->codeEdit()->appendPlainText(continuationDots);
            return true;
        }
        QTextCursor cursor = m_repl->codeEdit()->textCursor();

        switch (keyEvent->key())
        {
        case Qt::Key_Enter:
            Q_FALLTHROUGH();
        case Qt::Key_Return:
        {
            if (m_repl->codeEdit()->document()->characterCount() < replArrows.size())
            {
                return true;
            }

            // Try to be smart, create a new line if the python code will need one

            const int lastCharPos = m_repl->codeEdit()->document()->characterCount() - 2;
            if (m_repl->codeEdit()->document()->characterAt(lastCharPos) == ":")
            {
                m_repl->codeEdit()->appendPlainText(continuationDots);
                return true;
            }

            QString pythonCode = m_repl->codeEdit()->toPlainText();
            if (!m_repl->m_interpreter->isExecuting())
            {
                m_repl->executeCode(pythonCode);
                m_repl->m_history.add(std::move(pythonCode));
            }
            return true;
        }
        case Qt::Key_Backspace:
            Q_FALLTHROUGH();
        case Qt::Key_Delete:
            if (cursor.columnNumber() > replArrows.size())
            {
                return QObject::eventFilter(obj, event);
            }
            else
            {
                return true;
            }
        case Qt::Key_Left:
            if (cursor.columnNumber() > replArrows.size())
            {
                return QObject::eventFilter(obj, event);
            }
            return true;
        case Qt::Key_Down:
            if (cursor.blockNumber() < m_repl->codeEdit()->blockCount() - 1)
            {
                return QObject::eventFilter(obj, event);
            }
            else if (!m_repl->m_history.empty())
            {
                m_repl->codeEdit()->clear();
                m_repl->codeEdit()->insertPlainText(m_repl->m_history.newer());
            }
            return true;
        case Qt::Key_Up:
            if (cursor.blockNumber() > 0)
            {
                return QObject::eventFilter(obj, event);
            }
            else if (!m_repl->m_history.empty())
            {
                m_repl->codeEdit()->clear();
                m_repl->codeEdit()->insertPlainText(m_repl->m_history.older());
                return true;
            }
        case Qt::Key_Home:
            m_repl->codeEdit()->moveCursor(QTextCursor::StartOfLine);
            m_repl->codeEdit()->moveCursor(QTextCursor::NextWord);
            return true;
        default:
        {
            int posDiff = replArrows.size() - cursor.columnNumber();
            if (posDiff > 0)
            {
                cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, posDiff);
                m_repl->codeEdit()->setTextCursor(cursor);
            }
            return QObject::eventFilter(obj, event);
        }
        }
    }
    else
    {
        return QObject::eventFilter(obj, event);
    }
}

KeyPressEater::KeyPressEater(QPythonRepl *repl, QObject *parent) : QObject(parent), m_repl(repl) {}

QPythonRepl::QPythonRepl(PythonInterpreter *interpreter, QMainWindow *parent)
    : m_interpreter(interpreter), QMainWindow(parent), m_ui(new Ui_QPythonREPL), m_state()
{
    m_buf.reserve(255);

    setupUI();
    importNeededPackages();
}

QPythonRepl::~QPythonRepl() noexcept
{
    delete m_ui;
}

QPlainTextEdit *QPythonRepl::codeEdit()
{
    return m_ui->codeEdit;
}

QListWidget *QPythonRepl::outputDisplay()
{
    return m_ui->outputDisplay;
}

void QPythonRepl::setupUI()
{
    m_ui->setupUi(this);
    const auto keyPressEater = new KeyPressEater(this);

    const auto codeEditorHeight = static_cast<int>(height() * 0.25);
    m_ui->splitter->setSizes({height() - codeEditorHeight, codeEditorHeight});
    new PythonHighlighter(codeEdit()->document());

    codeEdit()->installEventFilter(keyPressEater);
    codeEdit()->resize(codeEdit()->width(), 20);
    codeEdit()->setTabStopWidth(codeEdit()->fontMetrics().width(' ') * 8);

    QFont font("Monospace");
    font.setStyleHint(QFont::TypeWriter);

    codeEdit()->setFont(font);

    connect(m_ui->toolBar->actions().at(0), &QAction::triggered, this, &QPythonRepl::reset);
}

void QPythonRepl::reset()
{
    m_state = PythonInterpreter::State();
    m_ui->outputDisplay->clear();
    importNeededPackages();
}

void QPythonRepl::importNeededPackages()
{
    executeCode(replArrows + "import pycc");
    executeCode(replArrows + "import cccorelib");
    executeCode(replArrows + "cc = pycc.GetInstance()");
}

void QPythonRepl::executeCode(const QString &pythonCode)
{
    /// Our raw input contains the ">>> " and "... " and is a QString
    /// convert it to std::string while filtering out non needed chars
    m_buf.clear();
    auto iter = pythonCode.begin() + replArrows.size();
    while (iter < pythonCode.end())
    {
        // FIXME this will return 0 on non latin chars
        //  we should do better
        const char c = iter->toLatin1();
        if (c == 0)
        {
            outputDisplay()->addItem("Input contains non Latin1 chars");
            return;
        }
        m_buf += c;
        if (*iter == '\n')
        {
            iter += continuationDots.size();
        }
        iter++;
    }
    outputDisplay()->addItem(pythonCode);
    codeEdit()->clear();
    codeEdit()->insertPlainText(replArrows);
    m_interpreter->executeCodeWithState(m_buf, outputDisplay(), m_state);
    outputDisplay()->scrollToBottom();
}

void History::add(QString &&cmd)
{
    m_commands.push_back(cmd);
    m_current = m_commands.rbegin();
}

const QString &History::older()
{
    if (m_current == m_commands.rend())
    {
        m_current = m_commands.rbegin();
    }
    const QString &current = *m_current;
    ++m_current;
    return current;
}

const QString &History::newer()
{
    if (m_current == m_commands.rend())
    {
        return replArrows;
    }

    const QString &current = *m_current;
    if (m_current == m_commands.rbegin())
    {
        m_current = m_commands.rend();
    }
    else
    {
        --m_current;
    }
    return current;
}

bool History::empty() const
{
    return m_commands.empty();
}

size_t History::size() const
{
    return m_commands.size();
}
