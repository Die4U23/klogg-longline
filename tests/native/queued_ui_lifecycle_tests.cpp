// Receiver-owned queued UI callback lifecycle regressions.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch.hpp>

#include <QCoreApplication>
#include <QApplication>
#include <QEvent>
#include <QIcon>
#include <QPointer>
#include <QPixmap>
#include <memory>

#include "highlightersetedit.h"
#include "highlightersdialog.h"
#include "predefinedfiltersdialog.h"
#include "tabbedcrawlerwidget.h"

namespace {
void drainPostedEvents()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
} // namespace

TEST_CASE( "HighlightersDialog queued initialization is canceled after immediate destruction",
           "[queued-ui][lifecycle]" )
{
    auto dialog = std::make_unique<HighlightersDialog>();
    QPointer<HighlightersDialog> guard( dialog.get() );
    dialog.reset();

    REQUIRE( guard.isNull() );
    drainPostedEvents();
    CHECK( guard.isNull() );
}

TEST_CASE( "HighlighterSetEdit queued initialization is canceled after immediate destruction",
           "[queued-ui][lifecycle]" )
{
    auto editor = std::make_unique<HighlighterSetEdit>();
    QPointer<HighlighterSetEdit> guard( editor.get() );
    editor.reset();

    REQUIRE( guard.isNull() );
    drainPostedEvents();
    CHECK( guard.isNull() );
}

TEST_CASE( "PredefinedFiltersDialog queued initialization is canceled after immediate destruction",
           "[queued-ui][lifecycle]" )
{
    auto dialog = std::make_unique<PredefinedFiltersDialog>();
    QPointer<PredefinedFiltersDialog> guard( dialog.get() );
    dialog.reset();

    REQUIRE( guard.isNull() );
    drainPostedEvents();
    CHECK( guard.isNull() );
}

TEST_CASE( "TabbedCrawlerWidget queued constructor and style callbacks are canceled on destruction",
           "[queued-ui][lifecycle]" )
{
    auto tabs = std::make_unique<TabbedCrawlerWidget>();
    QPointer<TabbedCrawlerWidget> guard( tabs.get() );
    QEvent styleChange( QEvent::StyleChange );
    QApplication::sendEvent( tabs.get(), &styleChange );
    tabs.reset();

    REQUIRE( guard.isNull() );
    drainPostedEvents();
    CHECK( guard.isNull() );
}

TEST_CASE( "HighlighterSetEdit applies queued icon initialization while it remains alive",
           "[queued-ui][initialization]" )
{
    HighlighterSetEdit editor;
    REQUIRE( editor.addHighlighterButton != nullptr );

    QPixmap marker( 2, 2 );
    marker.fill( Qt::magenta );
    const QIcon sentinel( marker );
    const auto sentinelKey = sentinel.cacheKey();
    editor.addHighlighterButton->setIcon( sentinel );

    drainPostedEvents();

    // Compare against a sentinel instead of requiring a non-null resource icon;
    // some test runners may not link the application's icon resources.
    CHECK( editor.addHighlighterButton->icon().cacheKey() != sentinelKey );
}
