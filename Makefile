RACK_DIR ?= /home/dev/Rack-SDK

FLAGS += -Ieurorack -DTEST
SOURCES += $(wildcard src/*.cpp)
SOURCES += eurorack/plaits/resources.cc
SOURCES += $(wildcard eurorack/plaits/dsp/*.cc)
SOURCES += $(wildcard eurorack/plaits/dsp/engine/*.cc)
SOURCES += $(wildcard eurorack/plaits/dsp/engine2/*.cc)
SOURCES += $(wildcard eurorack/plaits/dsp/chords/*.cc)
SOURCES += $(wildcard eurorack/plaits/dsp/speech/*.cc)
SOURCES += $(wildcard eurorack/plaits/dsp/physical_modelling/*.cc)
SOURCES += $(wildcard eurorack/plaits/dsp/fm/*.cc)
SOURCES += $(wildcard eurorack/plaits/dsp/drums/*.cc)
SOURCES += eurorack/stmlib/dsp/units.cc
SOURCES += eurorack/stmlib/dsp/atan.cc
SOURCES += eurorack/stmlib/utils/random.cc
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/plugin.mk
