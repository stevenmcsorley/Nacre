#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(modelNacre);
	p->addModel(modelAbacus);
	p->addModel(modelStiletto);
	p->addModel(modelAmmonite);
	p->addModel(modelBitrot);
	p->addModel(modelSirocco);
	p->addModel(modelEspalier);
	p->addModel(modelTektite);
	p->addModel(modelHaymaker);
	p->addModel(modelLariat);
	p->addModel(modelMaelstrom);
	p->addModel(modelChimera);
	p->addModel(modelCatgut);
	p->addModel(modelCapo);
	p->addModel(modelFretwork);
	p->addModel(modelMurmur);
	p->addModel(modelFieldfare);
	p->addModel(modelOsprey);
	p->addModel(modelFoxglove);
	p->addModel(modelRemora);
}
