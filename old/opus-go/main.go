package main

import (
	"bytes"
	"embed"
	"encoding/json"
	"errors"
	"fmt"
	"html/template"
	"io/fs"
	"log"
	"math"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
)

//go:embed static
var staticFS embed.FS

type Band struct {
	ID     string  `json:"id"`
	Type   string  `json:"type"`
	Freq   float64 `json:"freq"`
	Q      float64 `json:"q"`
	GainDB float64 `json:"gain_db"`
}

type TemplatePreset struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	SinkName    string `json:"sink_name"`
	SinkLabel   string `json:"sink_label"`
	Channels    int    `json:"channels"`
	Bands       []Band `json:"bands"`
}

type Config struct {
	Description   string           `json:"description"`
	SinkName      string           `json:"sink_name"`
	SinkLabel     string           `json:"sink_label"`
	Channels      int              `json:"channels"`
	AutoApply     bool             `json:"auto_apply"`
	ConfigPath    string           `json:"config_path"`
	StatePath     string           `json:"state_path"`
	LastApplyNote string           `json:"last_apply_note"`
	Bands         []Band           `json:"bands"`
	Templates     []TemplatePreset `json:"templates"`
}

type app struct {
	mu         sync.RWMutex
	cfg        Config
	cfgPath    string
	statePath  string
	lastRender string
}

type applyResult struct {
	OK      bool   `json:"ok"`
	Message string `json:"message"`
	Config  Config `json:"config"`
	Render  string `json:"render"`
}

type exchangeRequest struct {
	Format  string `json:"format"`
	Content string `json:"content"`
}

type exchangeResponse struct {
	OK      bool   `json:"ok"`
	Message string `json:"message"`
	Format  string `json:"format"`
	Content string `json:"content"`
	Config  Config `json:"config,omitempty"`
}

func main() {
	home, err := os.UserHomeDir()
	if err != nil {
		log.Fatalf("resolve home: %v", err)
	}

	stateDir := filepath.Join(home, ".local", "state", "eqweb")
	cfgPath := envOr("EQWEB_CONFIG_PATH", filepath.Join(home, ".config", "pipewire", "pipewire.conf.d", "99-eqweb.conf"))
	statePath := envOr("EQWEB_STATE_PATH", filepath.Join(stateDir, "config.json"))

	if err := os.MkdirAll(filepath.Dir(cfgPath), 0o755); err != nil {
		log.Fatalf("create pipewire config dir: %v", err)
	}
	if err := os.MkdirAll(filepath.Dir(statePath), 0o755); err != nil {
		log.Fatalf("create state dir: %v", err)
	}

	app := &app{
		cfgPath:   cfgPath,
		statePath: statePath,
		cfg:       defaultConfig(cfgPath, statePath),
	}

	if err := app.load(); err != nil {
		log.Printf("load config: %v", err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", app.handleIndex)
	mux.HandleFunc("/api/config", app.handleConfig)
	mux.HandleFunc("/api/apply", app.handleApply)
	mux.HandleFunc("/api/import", app.handleImport)
	mux.HandleFunc("/api/export", app.handleExport)

	sub, err := fs.Sub(staticFS, "static")
	if err != nil {
		log.Fatalf("static fs: %v", err)
	}
	mux.Handle("/static/", http.StripPrefix("/static/", http.FileServer(http.FS(sub))))

	addr := envOr("EQWEB_ADDR", "127.0.0.1:8932")
	log.Printf("serving eqweb on http://%s", addr)
	log.Fatal(http.ListenAndServe(addr, logRequest(mux)))
}

func defaultConfig(cfgPath, statePath string) Config {
	return Config{
		Description: "EQ Web Sink",
		SinkName:    "eqweb",
		SinkLabel:   "EQ Web Sink",
		Channels:    2,
		AutoApply:   false,
		ConfigPath:  cfgPath,
		StatePath:   statePath,
		Bands: []Band{
			{ID: "low", Type: "lowshelf", Freq: 80, Q: 0.707, GainDB: 0},
			{ID: "mid", Type: "peaking", Freq: 1000, Q: 1.0, GainDB: 0},
			{ID: "high", Type: "highshelf", Freq: 8000, Q: 0.707, GainDB: 0},
		},
	}
}

func (a *app) load() error {
	data, err := os.ReadFile(a.statePath)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		return err
	}

	var cfg Config
	if err := json.Unmarshal(data, &cfg); err != nil {
		return err
	}

	cfg.ConfigPath = a.cfgPath
	cfg.StatePath = a.statePath
	if err := normalizeConfig(&cfg); err != nil {
		return err
	}

	render, err := renderPipeWireConfig(cfg)
	if err != nil {
		return err
	}

	a.mu.Lock()
	a.cfg = cfg
	a.lastRender = render
	a.mu.Unlock()
	return nil
}

func (a *app) saveLocked() error {
	data, err := json.MarshalIndent(a.cfg, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(a.statePath, data, 0o644)
}

func (a *app) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}

	tpl, err := template.ParseFS(staticFS, "static/index.html")
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	a.mu.RLock()
	cfg := a.cfg
	a.mu.RUnlock()

	_ = tpl.Execute(w, map[string]any{
		"Title":   "EQ Web",
		"Address": envOr("EQWEB_ADDR", "127.0.0.1:8932"),
		"Config":  cfg,
	})
}

func (a *app) handleConfig(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		a.mu.RLock()
		cfg := a.cfg
		render := a.lastRender
		a.mu.RUnlock()
		writeJSON(w, http.StatusOK, applyResult{OK: true, Config: cfg, Render: render})
	case http.MethodPut:
		var cfg Config
		if err := json.NewDecoder(r.Body).Decode(&cfg); err != nil {
			writeJSON(w, http.StatusBadRequest, applyResult{OK: false, Message: "invalid json"})
			return
		}
		cfg.ConfigPath = a.cfgPath
		cfg.StatePath = a.statePath
		if err := normalizeConfig(&cfg); err != nil {
			writeJSON(w, http.StatusBadRequest, applyResult{OK: false, Message: err.Error()})
			return
		}
		render, err := renderPipeWireConfig(cfg)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, applyResult{OK: false, Message: err.Error()})
			return
		}

		a.mu.Lock()
		a.cfg = cfg
		a.lastRender = render
		if err := a.saveLocked(); err != nil {
			a.mu.Unlock()
			writeJSON(w, http.StatusInternalServerError, applyResult{OK: false, Message: err.Error()})
			return
		}
		a.mu.Unlock()

		if cfg.AutoApply {
			result := a.applyCurrent()
			status := http.StatusOK
			if !result.OK {
				status = http.StatusAccepted
			}
			writeJSON(w, status, result)
			return
		}

		writeJSON(w, http.StatusOK, applyResult{
			OK:      true,
			Message: "configuration saved",
			Config:  cfg,
			Render:  render,
		})
	default:
		w.WriteHeader(http.StatusMethodNotAllowed)
	}
}

func (a *app) handleApply(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	result := a.applyCurrent()
	status := http.StatusOK
	if !result.OK {
		status = http.StatusAccepted
	}
	writeJSON(w, status, result)
}

func (a *app) handleImport(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}

	var req exchangeRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, exchangeResponse{OK: false, Message: "invalid json"})
		return
	}

	a.mu.RLock()
	base := a.cfg
	a.mu.RUnlock()

	cfg, msg, err := importConfig(strings.TrimSpace(req.Format), req.Content, base, a.cfgPath, a.statePath)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, exchangeResponse{OK: false, Message: err.Error()})
		return
	}

	render, err := renderPipeWireConfig(cfg)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, exchangeResponse{OK: false, Message: err.Error()})
		return
	}

	a.mu.Lock()
	a.cfg = cfg
	a.lastRender = render
	if err := a.saveLocked(); err != nil {
		a.mu.Unlock()
		writeJSON(w, http.StatusInternalServerError, exchangeResponse{OK: false, Message: err.Error()})
		return
	}
	a.mu.Unlock()

	writeJSON(w, http.StatusOK, exchangeResponse{
		OK:      true,
		Message: msg,
		Format:  req.Format,
		Config:  cfg,
	})
}

func (a *app) handleExport(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}

	var req exchangeRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, exchangeResponse{OK: false, Message: "invalid json"})
		return
	}

	a.mu.RLock()
	cfg := a.cfg
	a.mu.RUnlock()

	content, err := exportConfig(strings.TrimSpace(req.Format), cfg)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, exchangeResponse{OK: false, Message: err.Error()})
		return
	}

	writeJSON(w, http.StatusOK, exchangeResponse{
		OK:      true,
		Message: "configuration exported",
		Format:  req.Format,
		Content: content,
		Config:  cfg,
	})
}

func (a *app) applyCurrent() applyResult {
	a.mu.Lock()
	defer a.mu.Unlock()

	render, err := renderPipeWireConfig(a.cfg)
	if err != nil {
		return applyResult{OK: false, Message: err.Error(), Config: a.cfg}
	}
	if err := os.WriteFile(a.cfgPath, []byte(render), 0o644); err != nil {
		return applyResult{OK: false, Message: err.Error(), Config: a.cfg, Render: render}
	}
	if err := a.saveLocked(); err != nil {
		return applyResult{OK: false, Message: err.Error(), Config: a.cfg, Render: render}
	}

	a.lastRender = render
	msg := "PipeWire config written. Restart PipeWire to apply."
	if err := restartUserAudio(); err == nil {
		msg = "PipeWire config written and user audio services restarted."
	} else {
		msg = msg + " Live restart failed: " + err.Error()
	}
	a.cfg.LastApplyNote = msg
	_ = a.saveLocked()

	return applyResult{OK: true, Message: msg, Config: a.cfg, Render: render}
}

func normalizeConfig(cfg *Config) error {
	cfg.Description = strings.TrimSpace(cfg.Description)
	cfg.SinkName = sanitizeName(cfg.SinkName)
	cfg.SinkLabel = strings.TrimSpace(cfg.SinkLabel)
	if cfg.Description == "" {
		cfg.Description = "EQ Web Sink"
	}
	if cfg.SinkName == "" {
		cfg.SinkName = "eqweb"
	}
	if cfg.SinkLabel == "" {
		cfg.SinkLabel = cfg.Description
	}
	if cfg.Channels <= 0 {
		cfg.Channels = 2
	}
	if cfg.Channels != 2 {
		return errors.New("only stereo output is supported right now")
	}
	if len(cfg.Bands) == 0 {
		return errors.New("at least one band is required")
	}

	seen := map[string]struct{}{}
	for i := range cfg.Bands {
		b := &cfg.Bands[i]
		b.ID = sanitizeName(b.ID)
		if b.ID == "" {
			b.ID = fmt.Sprintf("band%d", i+1)
		}
		if _, ok := seen[b.ID]; ok {
			b.ID = fmt.Sprintf("%s%d", b.ID, i+1)
		}
		seen[b.ID] = struct{}{}
		switch b.Type {
		case "peaking", "lowshelf", "highshelf", "lowpass", "highpass":
		default:
			return fmt.Errorf("band %q has unsupported type %q", b.ID, b.Type)
		}
		if b.Freq < 10 || b.Freq > 30000 {
			return fmt.Errorf("band %q frequency out of range", b.ID)
		}
		if b.Q <= 0.05 || b.Q > 20 {
			return fmt.Errorf("band %q Q out of range", b.ID)
		}
		if b.GainDB < -24 || b.GainDB > 24 {
			return fmt.Errorf("band %q gain out of range", b.ID)
		}
		if (b.Type == "lowpass" || b.Type == "highpass") && b.GainDB != 0 {
			b.GainDB = 0
		}
	}

	sort.Slice(cfg.Bands, func(i, j int) bool {
		if cfg.Bands[i].Freq == cfg.Bands[j].Freq {
			return cfg.Bands[i].ID < cfg.Bands[j].ID
		}
		return cfg.Bands[i].Freq < cfg.Bands[j].Freq
	})

	seenTemplates := map[string]struct{}{}
	for i := range cfg.Templates {
		tpl := &cfg.Templates[i]
		if err := normalizeTemplatePreset(tpl, i); err != nil {
			return err
		}
		if _, ok := seenTemplates[tpl.Name]; ok {
			tpl.Name = fmt.Sprintf("%s %d", tpl.Name, i+1)
		}
		seenTemplates[tpl.Name] = struct{}{}
	}
	return nil
}

func normalizeTemplatePreset(tpl *TemplatePreset, index int) error {
	tpl.Name = strings.TrimSpace(tpl.Name)
	if tpl.Name == "" {
		tpl.Name = fmt.Sprintf("Template %d", index+1)
	}

	cfg := Config{
		Description: tpl.Description,
		SinkName:    tpl.SinkName,
		SinkLabel:   tpl.SinkLabel,
		Channels:    tpl.Channels,
		Bands:       append([]Band(nil), tpl.Bands...),
	}
	if err := normalizeConfig(&cfg); err != nil {
		return fmt.Errorf("template %q: %w", tpl.Name, err)
	}

	tpl.Description = cfg.Description
	tpl.SinkName = cfg.SinkName
	tpl.SinkLabel = cfg.SinkLabel
	tpl.Channels = cfg.Channels
	tpl.Bands = cfg.Bands
	return nil
}

func renderPipeWireConfig(cfg Config) (string, error) {
	var nodes bytes.Buffer
	var links bytes.Buffer
	activeBands := make([]Band, 0, len(cfg.Bands))
	for _, b := range cfg.Bands {
		if (b.Type == "peaking" || b.Type == "lowshelf" || b.Type == "highshelf") && b.GainDB == 0 {
			continue
		}
		activeBands = append(activeBands, b)
	}

	for i, b := range activeBands {
		label := pipeWireLabel(b.Type)
		fmt.Fprintf(&nodes, "                    {\n")
		fmt.Fprintf(&nodes, "                        type  = builtin\n")
		fmt.Fprintf(&nodes, "                        name  = %s\n", b.ID)
		fmt.Fprintf(&nodes, "                        label = %s\n", label)
		if b.Type == "lowpass" || b.Type == "highpass" {
			fmt.Fprintf(&nodes, "                        control = { \"Freq\" = %.3f \"Q\" = %.3f }\n", b.Freq, b.Q)
		} else {
			fmt.Fprintf(&nodes, "                        control = { \"Freq\" = %.3f \"Q\" = %.3f \"Gain\" = %.3f }\n", b.Freq, b.Q, b.GainDB)
		}
		fmt.Fprintf(&nodes, "                    }\n")
		if i < len(activeBands)-1 {
			fmt.Fprintf(&links, "                    { output = \"%s:Out\" input = \"%s:In\" }\n", b.ID, activeBands[i+1].ID)
		}
	}

	var out bytes.Buffer
	out.WriteString("# generated by eqweb\n")
	out.WriteString("context.modules = [\n")
	out.WriteString("    { name = libpipewire-module-filter-chain\n")
	out.WriteString("        args = {\n")
	fmt.Fprintf(&out, "            node.description = %q\n", cfg.Description)
	fmt.Fprintf(&out, "            media.name       = %q\n", cfg.SinkLabel)
	out.WriteString("            filter.graph = {\n")
	out.WriteString("                nodes = [\n")
	out.Write(nodes.Bytes())
	out.WriteString("                ]\n")
	out.WriteString("                links = [\n")
	out.Write(links.Bytes())
	out.WriteString("                ]\n")
	out.WriteString("            }\n")
	fmt.Fprintf(&out, "            audio.channels = %d\n", cfg.Channels)
	out.WriteString("            audio.position = [ FL FR ]\n")
	out.WriteString("            capture.props = {\n")
	fmt.Fprintf(&out, "                node.name   = %q\n", "effect_input."+cfg.SinkName)
	out.WriteString("                media.class = Audio/Sink\n")
	out.WriteString("            }\n")
	out.WriteString("            playback.props = {\n")
	fmt.Fprintf(&out, "                node.name    = %q\n", "effect_output."+cfg.SinkName)
	out.WriteString("                node.passive = true\n")
	out.WriteString("            }\n")
	out.WriteString("        }\n")
	out.WriteString("    }\n")
	out.WriteString("]\n")
	return out.String(), nil
}

func pipeWireLabel(kind string) string {
	switch kind {
	case "lowshelf":
		return "bq_lowshelf"
	case "highshelf":
		return "bq_highshelf"
	case "lowpass":
		return "bq_lowpass"
	case "highpass":
		return "bq_highpass"
	default:
		return "bq_peaking"
	}
}

func restartUserAudio() error {
	cmds := [][]string{
		{"systemctl", "--user", "restart", "pipewire.service"},
		{"systemctl", "--user", "restart", "pipewire-pulse.service"},
		{"systemctl", "--user", "restart", "wireplumber.service"},
	}
	for _, parts := range cmds {
		cmd := exec.Command(parts[0], parts[1:]...)
		if out, err := cmd.CombinedOutput(); err != nil {
			return fmt.Errorf("%s: %s", strings.Join(parts, " "), strings.TrimSpace(string(out)))
		}
	}
	return nil
}

func importConfig(format, content string, base Config, cfgPath, statePath string) (Config, string, error) {
	format = strings.ToLower(strings.TrimSpace(format))
	var cfg Config
	switch format {
	case "eqweb-json":
		cfg = base
		if err := json.Unmarshal([]byte(content), &cfg); err != nil {
			return Config{}, "", fmt.Errorf("parse eqweb json: %w", err)
		}
	case "equalizer-apo":
		bands, err := parseEqualizerAPO(content)
		if err != nil {
			return Config{}, "", err
		}
		cfg = base
		cfg.Bands = bands
	case "graphic-eq":
		bands, err := parseGraphicEQ(content)
		if err != nil {
			return Config{}, "", err
		}
		cfg = base
		cfg.Bands = bands
	default:
		return Config{}, "", fmt.Errorf("unsupported import format %q", format)
	}
	cfg.ConfigPath = cfgPath
	cfg.StatePath = statePath
	if err := normalizeConfig(&cfg); err != nil {
		return Config{}, "", err
	}
	return cfg, fmt.Sprintf("imported %d bands from %s", len(cfg.Bands), format), nil
}

func exportConfig(format string, cfg Config) (string, error) {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "eqweb-json":
		data, err := json.MarshalIndent(cfg, "", "  ")
		if err != nil {
			return "", err
		}
		return string(data), nil
	case "equalizer-apo":
		return exportEqualizerAPO(cfg), nil
	case "graphic-eq":
		return exportGraphicEQ(cfg), nil
	default:
		return "", fmt.Errorf("unsupported export format %q", format)
	}
}

var apoLineRE = regexp.MustCompile(`(?i)^filter\s+\d+:\s*(?:on\s+)?([A-Z]+)\s+fc\s+([0-9.]+)\s*hz(?:\s+gain\s+([-0-9.]+)\s*dB)?(?:\s+q\s+([0-9.]+))?`)

func parseEqualizerAPO(content string) ([]Band, error) {
	lines := strings.Split(content, "\n")
	bands := make([]Band, 0, len(lines))
	for _, raw := range lines {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		m := apoLineRE.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		kind, err := apoTypeToBandType(strings.ToUpper(m[1]))
		if err != nil {
			return nil, err
		}
		freq, _ := strconv.ParseFloat(m[2], 64)
		gain := 0.0
		if m[3] != "" {
			gain, _ = strconv.ParseFloat(m[3], 64)
		}
		q := 0.707
		if m[4] != "" {
			q, _ = strconv.ParseFloat(m[4], 64)
		} else if kind == "peaking" {
			q = 1.0
		}
		bands = append(bands, Band{
			ID:     fmt.Sprintf("f%d", len(bands)+1),
			Type:   kind,
			Freq:   freq,
			Q:      q,
			GainDB: gain,
		})
	}
	if len(bands) == 0 {
		return nil, errors.New("no Equalizer APO filters were found")
	}
	return bands, nil
}

func apoTypeToBandType(token string) (string, error) {
	switch token {
	case "PK":
		return "peaking", nil
	case "LS", "LSC":
		return "lowshelf", nil
	case "HS", "HSC":
		return "highshelf", nil
	case "HP":
		return "highpass", nil
	case "LP":
		return "lowpass", nil
	default:
		return "", fmt.Errorf("unsupported Equalizer APO filter type %q", token)
	}
}

func parseGraphicEQ(content string) ([]Band, error) {
	text := strings.TrimSpace(content)
	if idx := strings.Index(strings.ToLower(text), "graphiceq:"); idx >= 0 {
		text = text[idx+len("GraphicEQ:"):]
	}
	chunks := strings.FieldsFunc(text, func(r rune) bool {
		return r == ';' || r == '\n' || r == '\r'
	})
	bands := make([]Band, 0, len(chunks))
	for _, chunk := range chunks {
		fields := strings.Fields(strings.TrimSpace(chunk))
		if len(fields) < 2 {
			continue
		}
		freq, err := strconv.ParseFloat(fields[0], 64)
		if err != nil {
			continue
		}
		gain, err := strconv.ParseFloat(fields[1], 64)
		if err != nil {
			continue
		}
		bands = append(bands, Band{
			ID:     fmt.Sprintf("g%d", len(bands)+1),
			Type:   "peaking",
			Freq:   freq,
			Q:      guessGraphicQQ(len(bands), chunks, freq),
			GainDB: gain,
		})
	}
	if len(bands) == 0 {
		return nil, errors.New("no GraphicEQ points were found")
	}
	return bands, nil
}

func guessGraphicQQ(idx int, chunks []string, freq float64) float64 {
	freqs := make([]float64, 0, len(chunks))
	for _, chunk := range chunks {
		fields := strings.Fields(strings.TrimSpace(chunk))
		if len(fields) < 2 {
			continue
		}
		f, err := strconv.ParseFloat(fields[0], 64)
		if err == nil {
			freqs = append(freqs, f)
		}
	}
	if len(freqs) < 2 {
		return 1.414
	}
	left := freq
	right := freq
	if idx > 0 && idx-1 < len(freqs) {
		left = freqs[idx-1]
	}
	if idx+1 < len(freqs) {
		right = freqs[idx+1]
	}
	ratio := 2.0
	switch {
	case left != freq && right != freq:
		ratio = math.Sqrt(right / left)
	case right != freq:
		ratio = right / freq
	case left != freq:
		ratio = freq / left
	}
	if ratio <= 1 {
		return 1.414
	}
	bw := math.Log(ratio) / math.Log(2)
	if bw <= 0 {
		return 1.414
	}
	q := math.Sqrt(math.Pow(2, bw)) / (math.Pow(2, bw) - 1)
	if q < 0.2 || q > 8 {
		return 1.414
	}
	return q
}

func exportEqualizerAPO(cfg Config) string {
	lines := []string{
		"# exported by eqweb",
		"Preamp: 0 dB",
	}
	for i, b := range cfg.Bands {
		token := bandTypeToAPO(b.Type)
		line := fmt.Sprintf("Filter %d: ON %s Fc %.3f Hz", i+1, token, b.Freq)
		if b.Type != "lowpass" && b.Type != "highpass" {
			line += fmt.Sprintf(" Gain %.3f dB", b.GainDB)
		}
		line += fmt.Sprintf(" Q %.3f", b.Q)
		lines = append(lines, line)
	}
	return strings.Join(lines, "\n") + "\n"
}

func bandTypeToAPO(kind string) string {
	switch kind {
	case "lowshelf":
		return "LS"
	case "highshelf":
		return "HS"
	case "lowpass":
		return "LP"
	case "highpass":
		return "HP"
	default:
		return "PK"
	}
}

func exportGraphicEQ(cfg Config) string {
	parts := make([]string, 0, len(cfg.Bands))
	for _, b := range cfg.Bands {
		parts = append(parts, fmt.Sprintf("%.3f %.3f", b.Freq, b.GainDB))
	}
	return "GraphicEQ: " + strings.Join(parts, "; ") + "\n"
}

func sanitizeName(s string) string {
	s = strings.ToLower(strings.TrimSpace(s))
	var b strings.Builder
	for _, r := range s {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') {
			b.WriteRune(r)
			continue
		}
		if r == '-' || r == '_' {
			b.WriteRune(r)
			continue
		}
	}
	return b.String()
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func envOr(key, fallback string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return fallback
}

func logRequest(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		log.Printf("%s %s", r.Method, r.URL.Path)
		next.ServeHTTP(w, r)
	})
}
