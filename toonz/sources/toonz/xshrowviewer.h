#pragma once

#ifndef XSHROWVIEWER_H
#define XSHROWVIEWER_H

#include <QWidget>

// forward declaration
class XsheetViewer;
class QMenu;

namespace XsheetGUI {

class DragTool;

//=============================================================================
// RowArea
//-----------------------------------------------------------------------------

//! La classe si occupa della visualizzazione dell'area che gestisce le righe.
class RowArea final : public QWidget {
  Q_OBJECT
  XsheetViewer *m_viewer;
  int m_row;

  enum ShowOnionToSetFlag {
    None = 0,
    Fos,
    Mos,
    ShiftTraceGhost
  } m_showOnionToSet;

  enum Direction { up = 0, down };

  // Play ranges
  int m_r0;
  int m_r1;

  QPoint m_pos;
  bool m_playRangeActiveInMousePress;
  int m_mousePressRow;
  QString m_tooltip;

  // panning by middle-drag
  bool m_isPanning;

  QTimer *m_resetMenuTimer;
  int m_contextMenuRow;
  bool m_editTagEnabled;

  // returns true if the frame area can have extra space
  bool checkExpandFrameArea();
  void drawRows(QPainter &p, int r0, int r1);
  void drawPlayRangeBackground(QPainter &p, int r0, int r1);
  void drawPlayRange(QPainter &p, int r0, int r1);
  void drawCurrentRowGadget(QPainter &p, int r0, int r1);
  void drawOnionSkinBackground(QPainter &p, int r0, int r1);
  void drawOnionSkinSelection(QPainter &p);
  void drawPinnedCenterKeys(QPainter &p, int r0, int r1);
  void drawCurrentTimeIndicator(QPainter &p);
  void drawCurrentTimeLine(QPainter &p);
  void drawShiftTraceMarker(QPainter &p);
  void drawStopMotionCameraIndicator(QPainter &p);
  void drawNavigationTags(QPainter &p, int r0, int r1);

  DragTool *getDragTool() const;
  void setDragTool(DragTool *dragTool);

  // Return when the item-menu setAutoMarkers can be enabled.
  bool canSetAutoMarkers();

public:
  RowArea(XsheetViewer *parent, Qt::WindowFlags flags = Qt::WindowFlags());
  ~RowArea();

  int getContextMenuRow() { return m_contextMenuRow; }

protected:
  void paintEvent(QPaintEvent *) override;

  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  bool event(QEvent *event) override;

protected slots:
  void onJumpToTag();
  void onHideMenu();
  void resetContextMenu();
};

}  // namespace XsheetGUI;

#endif  // XSHROWVIEWER_H
