/* $Id$ */
/** @file
 * VBox Qt GUI - UIToolBar class implementation.
 */

/*
 * Copyright (C) 2006-2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifdef VBOX_WITH_PRECOMPILED_HEADERS
# include <precomp.h>
#else  /* !VBOX_WITH_PRECOMPILED_HEADERS */

/* Qt includes: */
# include <QLayout>
# include <QMainWindow>

/* GUI includes: */
# include "UIToolBar.h"
# ifdef Q_WS_MAC
#  include "VBoxUtils.h"
# endif /* !VBOX_WITH_PRECOMPILED_HEADERS */

#endif /* Q_WS_MAC */

/* Qt includes: */
#if QT_VERSION < 0x050000
# include <QWindowsStyle>
# include <QCleanlooksStyle>
#endif /* QT_VERSION < 0x050000 */


UIToolBar::UIToolBar(QWidget *pParent /* = 0 */)
    : QToolBar(pParent)
    , m_pMainWindow(qobject_cast<QMainWindow*>(pParent))
{
    /* Prepare: */
    prepare();
}

void UIToolBar::setUseTextLabels(bool fEnable)
{
    /* Determine tool-button style on the basis of passed flag: */
    Qt::ToolButtonStyle tbs = fEnable ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly;

    /* Depending on parent, assign this style: */
    if (m_pMainWindow)
        m_pMainWindow->setToolButtonStyle(tbs);
    else
        setToolButtonStyle(tbs);
}

#ifdef Q_WS_MAC
void UIToolBar::enableMacToolbar()
{
    /* Depending on parent, enable unified title/tool-bar: */
    if (m_pMainWindow)
        m_pMainWindow->setUnifiedTitleAndToolBarOnMac(true);
}

void UIToolBar::setShowToolBarButton(bool fShow)
{
    ::darwinSetShowsToolbarButton(this, fShow);
}

void UIToolBar::updateLayout()
{
    /* There is a bug in Qt Cocoa which result in showing a "more arrow" when
       the necessary size of the toolbar is increased. Also for some languages
       the with doesn't match if the text increase. So manually adjust the size
       after changing the text. */
    QSizePolicy sp = sizePolicy();
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    adjustSize();
    setSizePolicy(sp);
    layout()->invalidate();
    layout()->activate();
}
#endif /* Q_WS_MAC */

void UIToolBar::prepare()
{
    /* Configure tool-bar: */
    setFloatable(false);
    setMovable(false);

#if QT_VERSION < 0x050000
    /* Remove that ugly frame panel around the toolbar.
     * Doing that currently for Cleanlooks & Windows styles. */
    if (qobject_cast <QWindowsStyle*>(QToolBar::style()) ||
        qobject_cast <QCleanlooksStyle*>(QToolBar::style()))
        setStyleSheet("QToolBar { border: 0px none black; }");
#else /* QT_VERSION >= 0x050000 */
# ifdef Q_WS_MAC
        setStyleSheet("QToolBar { border: 0px none black; }");
# endif /* Q_WS_MAC */
#endif /* QT_VERSION >= 0x050000 */

    /* Configure tool-bar' layout: */
    if (layout())
        layout()->setContentsMargins(0, 0, 0, 0);

    /* Configure tool-bar' context-menu policy: */
    setContextMenuPolicy(Qt::NoContextMenu);
}

