/********************************************************************
**  Nulloy Music Player, http://nulloy.com
**  Copyright (C) 2010-2011 Sergey Vlasov <sergey@vlasov.me>
**
**  This program can be distributed under the terms of the GNU
**  General Public License version 3.0 as published by the Free
**  Software Foundation and appearing in the file LICENSE.GPL3
**  included in the packaging of this file.  Please review the
**  following information to ensure the GNU General Public License
**  version 3.0 requirements will be met:
**
**  http://www.gnu.org/licenses/gpl-3.0.html
**
*********************************************************************/

#ifndef N_PLAYLIST_ITEM_H
#define N_PLAYLIST_ITEM_H

#include <QListWidgetItem>

class NPlaylistItem : public QListWidgetItem
{
private:
	bool m_failed;
	QString m_path;

public:
	enum PlaylistRole {
		FailedRole = Qt::UserRole + 1,
		PathRole
	};

	NPlaylistItem(QListWidget *parent = 0);

	QVariant data(int role) const;
	void setData(int role, const QVariant &value);
};

#include <QItemDelegate>

class NPlaylistItemDelegate : public QItemDelegate
{
	Q_OBJECT

public:
	NPlaylistItemDelegate(QWidget *parent = 0) : QItemDelegate(parent) {}
	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
};


#endif

/* vim: set ts=4 sw=4: */