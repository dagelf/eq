package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRenderPipeWireConfig(t *testing.T) {
	cfg := defaultConfig("/tmp/out.conf", "/tmp/state.json")
	cfg.SinkName = "desk"
	cfg.Bands = []Band{
		{ID: "a", Type: "lowshelf", Freq: 90, Q: 0.7, GainDB: -2},
		{ID: "b", Type: "peaking", Freq: 1000, Q: 1.1, GainDB: 3.5},
		{ID: "c", Type: "highpass", Freq: 40, Q: 0.8, GainDB: 4},
	}
	if err := normalizeConfig(&cfg); err != nil {
		t.Fatalf("normalize: %v", err)
	}
	render, err := renderPipeWireConfig(cfg)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	for _, want := range []string{
		`label = bq_lowshelf`,
		`label = bq_peaking`,
		`label = bq_highpass`,
		`"Gain" = 3.500`,
		`control = { "Freq" = 40.000 "Q" = 0.800 }`,
		`node.name   = "effect_input.desk"`,
	} {
		if !strings.Contains(render, want) {
			t.Fatalf("render missing %q\n%s", want, render)
		}
	}
}

func TestRenderPipeWireConfigSkipsZeroGainBands(t *testing.T) {
	cfg := defaultConfig("/tmp/out.conf", "/tmp/state.json")
	cfg.Bands = []Band{
		{ID: "low", Type: "lowshelf", Freq: 80, Q: 0.707, GainDB: 0},
		{ID: "mid", Type: "peaking", Freq: 1000, Q: 1.0, GainDB: 2.5},
		{ID: "high", Type: "highshelf", Freq: 8000, Q: 0.707, GainDB: 0},
	}
	if err := normalizeConfig(&cfg); err != nil {
		t.Fatalf("normalize: %v", err)
	}

	render, err := renderPipeWireConfig(cfg)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	if strings.Contains(render, "name  = low") || strings.Contains(render, "name  = high") {
		t.Fatalf("zero-gain bands should be omitted:\n%s", render)
	}
	if !strings.Contains(render, "name  = mid") {
		t.Fatalf("active band missing:\n%s", render)
	}
}

func TestHandleConfigAndApply(t *testing.T) {
	tmp := t.TempDir()
	app := &app{
		cfgPath:   filepath.Join(tmp, "pipewire.conf.d", "99-eqweb.conf"),
		statePath: filepath.Join(tmp, "state", "config.json"),
		cfg:       defaultConfig(filepath.Join(tmp, "pipewire.conf.d", "99-eqweb.conf"), filepath.Join(tmp, "state", "config.json")),
	}
	if err := os.MkdirAll(filepath.Dir(app.cfgPath), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(app.statePath), 0o755); err != nil {
		t.Fatal(err)
	}

	body := `{"description":"Desk EQ","sink_name":"desk-eq","sink_label":"Desk EQ","channels":2,"auto_apply":false,"bands":[{"id":"one","type":"peaking","freq":120,"q":0.8,"gain_db":2.5}]}`
	req := httptest.NewRequest(http.MethodPut, "/api/config", strings.NewReader(body))
	rec := httptest.NewRecorder()
	app.handleConfig(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("save status = %d body=%s", rec.Code, rec.Body.String())
	}

	req = httptest.NewRequest(http.MethodPost, "/api/apply", nil)
	rec = httptest.NewRecorder()
	app.handleApply(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("apply status = %d body=%s", rec.Code, rec.Body.String())
	}
	var res applyResult
	if err := json.Unmarshal(rec.Body.Bytes(), &res); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(res.Render, `label = bq_peaking`) {
		t.Fatalf("unexpected render: %s", res.Render)
	}
	out, err := os.ReadFile(app.cfgPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(out), `node.name   = "effect_input.desk-eq"`) {
		t.Fatalf("written config missing sink name:\n%s", string(out))
	}
}

func TestNormalizeConfigTemplates(t *testing.T) {
	cfg := defaultConfig("/tmp/out.conf", "/tmp/state.json")
	cfg.Templates = []TemplatePreset{
		{
			Name:        "Desk",
			Description: "Desk",
			SinkName:    "desk",
			SinkLabel:   "Desk",
			Channels:    2,
			Bands: []Band{
				{ID: "peak", Type: "peaking", Freq: 1000, Q: 1.0, GainDB: 3},
			},
		},
		{
			Name:        "Desk",
			Description: "Cinema",
			SinkName:    "cinema",
			SinkLabel:   "Cinema",
			Channels:    2,
			Bands: []Band{
				{ID: "hp", Type: "highpass", Freq: 35, Q: 0.8, GainDB: 4},
			},
		},
	}

	if err := normalizeConfig(&cfg); err != nil {
		t.Fatalf("normalize: %v", err)
	}
	if len(cfg.Templates) != 2 {
		t.Fatalf("got %d templates", len(cfg.Templates))
	}
	if cfg.Templates[1].Name != "Desk 2" {
		t.Fatalf("expected duplicate template name to be renamed, got %q", cfg.Templates[1].Name)
	}
	if cfg.Templates[1].Bands[0].GainDB != 0 {
		t.Fatalf("expected highpass gain to normalize to 0, got %v", cfg.Templates[1].Bands[0].GainDB)
	}
}

func TestParseEqualizerAPO(t *testing.T) {
	input := `
Preamp: -2 dB
Filter 1: ON PK Fc 105 Hz Gain -3.2 dB Q 0.90
Filter 2: ON HS Fc 8500 Hz Gain 4.5 dB Q 0.70
Filter 3: ON HP Fc 35 Hz Q 0.80
`
	bands, err := parseEqualizerAPO(input)
	if err != nil {
		t.Fatal(err)
	}
	if len(bands) != 3 {
		t.Fatalf("got %d bands", len(bands))
	}
	if bands[0].Type != "peaking" || bands[1].Type != "highshelf" || bands[2].Type != "highpass" {
		t.Fatalf("unexpected band types: %#v", bands)
	}
	if bands[2].GainDB != 0 {
		t.Fatalf("expected highpass gain to default to 0, got %v", bands[2].GainDB)
	}
}

func TestParseGraphicEQ(t *testing.T) {
	input := `GraphicEQ: 20 0; 40 -2.5; 80 -1.0; 160 1.2`
	bands, err := parseGraphicEQ(input)
	if err != nil {
		t.Fatal(err)
	}
	if len(bands) != 4 {
		t.Fatalf("got %d bands", len(bands))
	}
	for i, b := range bands {
		if b.Type != "peaking" {
			t.Fatalf("band %d type = %s", i, b.Type)
		}
		if b.Q <= 0 {
			t.Fatalf("band %d has invalid q %v", i, b.Q)
		}
	}
}

func TestExportEqualizerAPO(t *testing.T) {
	cfg := defaultConfig("/tmp/out.conf", "/tmp/state.json")
	cfg.Bands = []Band{
		{ID: "ls", Type: "lowshelf", Freq: 80, Q: 0.71, GainDB: 2},
		{ID: "pk", Type: "peaking", Freq: 1200, Q: 1.2, GainDB: -1.5},
		{ID: "hp", Type: "highpass", Freq: 30, Q: 0.8, GainDB: 0},
	}
	out := exportEqualizerAPO(cfg)
	for _, want := range []string{
		"Filter 1: ON LS Fc 80.000 Hz Gain 2.000 dB Q 0.710",
		"Filter 2: ON PK Fc 1200.000 Hz Gain -1.500 dB Q 1.200",
		"Filter 3: ON HP Fc 30.000 Hz Q 0.800",
	} {
		if !strings.Contains(out, want) {
			t.Fatalf("missing %q in\n%s", want, out)
		}
	}
}

func TestImportExportEndpoints(t *testing.T) {
	tmp := t.TempDir()
	app := &app{
		cfgPath:   filepath.Join(tmp, "pipewire.conf.d", "99-eqweb.conf"),
		statePath: filepath.Join(tmp, "state", "config.json"),
		cfg:       defaultConfig(filepath.Join(tmp, "pipewire.conf.d", "99-eqweb.conf"), filepath.Join(tmp, "state", "config.json")),
	}
	if err := os.MkdirAll(filepath.Dir(app.cfgPath), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(app.statePath), 0o755); err != nil {
		t.Fatal(err)
	}

	importBody := `{"format":"equalizer-apo","content":"Filter 1: ON PK Fc 250 Hz Gain 1.5 dB Q 1.00\nFilter 2: ON HS Fc 6000 Hz Gain -2.0 dB Q 0.70"}`
	req := httptest.NewRequest(http.MethodPost, "/api/import", strings.NewReader(importBody))
	rec := httptest.NewRecorder()
	app.handleImport(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("import status = %d body=%s", rec.Code, rec.Body.String())
	}

	req = httptest.NewRequest(http.MethodPost, "/api/export", strings.NewReader(`{"format":"graphic-eq"}`))
	rec = httptest.NewRecorder()
	app.handleExport(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("export status = %d body=%s", rec.Code, rec.Body.String())
	}
	var res exchangeResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &res); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(res.Content, "GraphicEQ:") {
		t.Fatalf("unexpected export content: %s", res.Content)
	}
}

func TestHandleConfigPersistsTemplates(t *testing.T) {
	tmp := t.TempDir()
	app := &app{
		cfgPath:   filepath.Join(tmp, "pipewire.conf.d", "99-eqweb.conf"),
		statePath: filepath.Join(tmp, "state", "config.json"),
		cfg:       defaultConfig(filepath.Join(tmp, "pipewire.conf.d", "99-eqweb.conf"), filepath.Join(tmp, "state", "config.json")),
	}
	if err := os.MkdirAll(filepath.Dir(app.cfgPath), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(app.statePath), 0o755); err != nil {
		t.Fatal(err)
	}

	body := `{"description":"Desk EQ","sink_name":"desk-eq","sink_label":"Desk EQ","channels":2,"auto_apply":false,"bands":[{"id":"one","type":"peaking","freq":120,"q":0.8,"gain_db":2.5}],"templates":[{"name":"Desk","description":"Desk EQ","sink_name":"desk-eq","sink_label":"Desk EQ","channels":2,"bands":[{"id":"one","type":"peaking","freq":120,"q":0.8,"gain_db":2.5}]}]}`
	req := httptest.NewRequest(http.MethodPut, "/api/config", strings.NewReader(body))
	rec := httptest.NewRecorder()
	app.handleConfig(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("save status = %d body=%s", rec.Code, rec.Body.String())
	}

	saved, err := os.ReadFile(app.statePath)
	if err != nil {
		t.Fatal(err)
	}

	var cfg Config
	if err := json.Unmarshal(saved, &cfg); err != nil {
		t.Fatal(err)
	}
	if len(cfg.Templates) != 1 {
		t.Fatalf("expected 1 template, got %d", len(cfg.Templates))
	}
	if cfg.Templates[0].Name != "Desk" {
		t.Fatalf("unexpected template name %q", cfg.Templates[0].Name)
	}
}
