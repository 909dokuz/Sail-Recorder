QT += core dbus
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle

TEMPLATE = app
TARGET = sail-recorder-dbus-bridge

SOURCES += sail-recorder-dbus-bridge.cpp

QMAKE_CXXFLAGS += -O2 -Wall -Wextra
