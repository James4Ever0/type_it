// golang_demo is a Go/Ebiten port of the macro keyboard prototype.
// It reads profiles from ./profiles, shows them on a simulated LED display,
// and uses a clickable/draggable knob (or keyboard) to navigate and copy.
package main

import (
	"encoding/json"
	"fmt"
	"image/color"
	"log"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/atotto/clipboard"
	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/inpututil"
	"github.com/hajimehoshi/ebiten/v2/text"
	"github.com/hajimehoshi/ebiten/v2/vector"
	"golang.org/x/image/font"
	"golang.org/x/image/font/gofont/goregular"
	"golang.org/x/image/font/opentype"
)

const (
	profilesDir = "./profiles"
	usageFile   = "./usage.json"

	screenW, screenH = 273, 416
	fps              = 60

	deviceX, deviceY = 20, 20
	deviceW          = screenW - 40
	deviceH          = screenH - 40
	deviceRadius     = 13

	screenMargin = 16
	displayX     = deviceX + screenMargin
	displayY     = deviceY + screenMargin
	displayW     = deviceW - screenMargin*2
	displayH     = 208

	knobMargin  = 17
	knobCenterX = screenW / 2
	knobCenterY = displayY + displayH + (deviceY+deviceH-displayY-displayH)/2
	knobRadius  = min((deviceY+deviceH-displayY-displayH-knobMargin*2)/2, displayW/2-knobMargin)
)

var (
	knobInnerR = knobRadius * 72 / 100

	longPressMs int64 = 600
	knobStepRad       = 0.35 // ~20 degrees per detent
)

var (
	colorBG            = color.NRGBA{40, 42, 46, 255}
	colorDevice        = color.NRGBA{30, 32, 36, 255}
	colorDisplayBG     = color.NRGBA{10, 12, 14, 255}
	colorDisplayBorder = color.NRGBA{60, 64, 72, 255}
	colorText          = color.NRGBA{200, 210, 220, 255}
	colorTextDim       = color.NRGBA{100, 110, 120, 255}
	colorHighlight     = color.NRGBA{70, 130, 180, 255}
	colorKnob          = color.NRGBA{80, 84, 92, 255}
	colorKnobFace      = color.NRGBA{110, 115, 125, 255}
	colorKnobShadow    = color.NRGBA{50, 52, 58, 255}
	colorIndicator     = color.NRGBA{40, 44, 48, 255}
)

type candidate struct {
	Text     string
	LastUsed int
}

type profile struct {
	Name       string
	Candidates []candidate
	LastUsed   int
}

// UsageStore keeps all ordering state in memory and writes it to disk on Save.
type UsageStore struct {
	Profiles   map[string]int `json:"profiles"`
	Candidates map[string]int `json:"candidates"`
	KnobAngle  float64        `json:"knob_angle"`
}

func newUsageStore(path string) *UsageStore {
	s := &UsageStore{
		Profiles:   map[string]int{},
		Candidates: map[string]int{},
		KnobAngle:  -math.Pi / 2,
	}
	data, err := os.ReadFile(path)
	if err == nil {
		_ = json.Unmarshal(data, s)
	}
	return s
}

func (s *UsageStore) allValues() []int {
	vals := make([]int, 0, len(s.Profiles)+len(s.Candidates))
	for _, v := range s.Profiles {
		vals = append(vals, v)
	}
	for _, v := range s.Candidates {
		vals = append(vals, v)
	}
	return vals
}

func (s *UsageStore) nextOrder() int {
	vals := s.allValues()
	if len(vals) == 0 {
		return 1
	}
	max := vals[0]
	for _, v := range vals[1:] {
		if v > max {
			max = v
		}
	}
	return max + 1
}

func (s *UsageStore) normalize() {
	values := make(map[int]struct{})
	for _, v := range s.Profiles {
		values[v] = struct{}{}
	}
	for _, v := range s.Candidates {
		values[v] = struct{}{}
	}
	if len(values) == 0 {
		return
	}
	sorted := make([]int, 0, len(values))
	for v := range values {
		sorted = append(sorted, v)
	}
	sort.Ints(sorted)
	mapping := make(map[int]int, len(sorted))
	for i, v := range sorted {
		mapping[v] = i
	}
	for k, v := range s.Profiles {
		s.Profiles[k] = mapping[v]
	}
	for k, v := range s.Candidates {
		s.Candidates[k] = mapping[v]
	}
}

func (s *UsageStore) Save(path string) error {
	s.normalize()
	data, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0644)
}

func (s *UsageStore) profileOrder(name string) int {
	if v, ok := s.Profiles[name]; ok {
		return v
	}
	return 0
}

func (s *UsageStore) candidateOrder(profileName, text string) int {
	if v, ok := s.Candidates[profileName+"::"+text]; ok {
		return v
	}
	return 0
}

func (s *UsageStore) touchProfile(name string) int {
	order := s.nextOrder()
	s.Profiles[name] = order
	return order
}

func (s *UsageStore) touchCandidate(profileName, text string) int {
	order := s.nextOrder()
	s.Candidates[profileName+"::"+text] = order
	return order
}

func loadProfiles(store *UsageStore) []profile {
	profiles := []profile{}
	entries, err := os.ReadDir(profilesDir)
	if err != nil {
		_ = os.MkdirAll(profilesDir, 0755)
		return profiles
	}
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".txt") {
			continue
		}
		name := strings.TrimSuffix(entry.Name(), ".txt")
		data, err := os.ReadFile(filepath.Join(profilesDir, entry.Name()))
		if err != nil {
			continue
		}
		seen := map[string]struct{}{}
		cands := []candidate{}
		for _, line := range strings.Split(string(data), "\n") {
			text := strings.TrimSpace(line)
			if text == "" {
				continue
			}
			if _, ok := seen[text]; ok {
				continue
			}
			seen[text] = struct{}{}
			cands = append(cands, candidate{
				Text:     text,
				LastUsed: store.candidateOrder(name, text),
			})
		}
		sort.Slice(cands, func(i, j int) bool { return cands[i].LastUsed > cands[j].LastUsed })
		profiles = append(profiles, profile{
			Name:       name,
			Candidates: cands,
			LastUsed:   store.profileOrder(name),
		})
	}
	sort.Slice(profiles, func(i, j int) bool { return profiles[i].LastUsed > profiles[j].LastUsed })
	return profiles
}

func findCJKFont() string {
	candidates := []string{
		"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
		"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
		"/usr/share/fonts/opentype/source-han-sans/SourceHanSansCN-Regular.otf",
		"/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	}
	for _, p := range candidates {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return ""
}

type Game struct {
	profiles        []profile
	store           *UsageStore
	view            string // "profiles" or "candidates"
	profileIndex    int
	candidateIndex  int
	knobAngle       float64
	knobAccum       float64
	knobPressed     bool
	pressStartMs    int64
	longTriggered   bool
	holdInvalidated bool
	flashUntilMs    int64
	flashText       string

	// mouse drag
	mouseOnKnob   bool
	dragStartX    float64
	dragStartY    float64
	isDragging    bool
	lastDragAngle float64

	fontTitle font.Face
	fontBody  font.Face
}

func loadTextFace(path string, size float64) (font.Face, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return loadTextFaceFromBytes(data, size)
}

func loadTextFaceFromBytes(data []byte, size float64) (font.Face, error) {
	var f *opentype.Font
	if len(data) >= 4 && string(data[:4]) == "ttcf" {
		coll, err := opentype.ParseCollection(data)
		if err != nil {
			return nil, err
		}
		f, err = coll.Font(0)
		if err != nil {
			return nil, err
		}
	} else {
		var err error
		f, err = opentype.Parse(data)
		if err != nil {
			return nil, err
		}
	}
	face, err := opentype.NewFace(f, &opentype.FaceOptions{Size: size, DPI: 72})
	if err != nil {
		return nil, err
	}
	return face, nil
}

func newGame() *Game {
	store := newUsageStore(usageFile)
	profiles := loadProfiles(store)
	angle := store.KnobAngle
	if angle == 0 {
		angle = -math.Pi / 2
	}

	var titleFace, bodyFace font.Face
	fontPath := findCJKFont()
	if fontPath != "" {
		if f, err := loadTextFace(fontPath, 14); err == nil {
			titleFace = f
		} else {
			log.Printf("failed to load title font %s: %v", fontPath, err)
		}
		if f, err := loadTextFace(fontPath, 12); err == nil {
			bodyFace = f
		} else {
			log.Printf("failed to load body font %s: %v", fontPath, err)
		}
	}
	if titleFace == nil {
		if f, err := loadTextFaceFromBytes(goregular.TTF, 14); err == nil {
			titleFace = f
		}
	}
	if bodyFace == nil {
		if f, err := loadTextFaceFromBytes(goregular.TTF, 12); err == nil {
			bodyFace = f
		}
	}
	if titleFace == nil || bodyFace == nil {
		log.Fatal("failed to load any font")
	}

	return &Game{
		profiles:  profiles,
		store:     store,
		view:      "profiles",
		knobAngle: angle,
		fontTitle: titleFace,
		fontBody:  bodyFace,
	}
}

func (g *Game) currentProfile() *profile {
	if g.profileIndex >= 0 && g.profileIndex < len(g.profiles) {
		return &g.profiles[g.profileIndex]
	}
	return nil
}

func (g *Game) openCurrentProfile() {
	prof := g.currentProfile()
	if prof == nil || len(prof.Candidates) == 0 {
		return
	}
	name := prof.Name
	prof.LastUsed = g.store.touchProfile(name)
	sort.Slice(g.profiles, func(i, j int) bool { return g.profiles[i].LastUsed > g.profiles[j].LastUsed })
	g.profileIndex = 0
	for i, p := range g.profiles {
		if p.Name == name {
			g.profileIndex = i
			break
		}
	}
	g.view = "candidates"
	g.candidateIndex = 0
	_ = g.store.Save(usageFile)
	g.syncLastUsed()
}

func (g *Game) copyCurrentCandidate() {
	prof := g.currentProfile()
	if prof == nil {
		return
	}
	if g.candidateIndex < 0 || g.candidateIndex >= len(prof.Candidates) {
		return
	}
	cand := &prof.Candidates[g.candidateIndex]
	name := prof.Name
	copiedText := cand.Text
	_ = clipboard.WriteAll(copiedText)

	cand.LastUsed = g.store.touchCandidate(name, copiedText)
	g.store.touchProfile(name)
	prof.LastUsed = cand.LastUsed

	sort.Slice(prof.Candidates, func(i, j int) bool { return prof.Candidates[i].LastUsed > prof.Candidates[j].LastUsed })
	sort.Slice(g.profiles, func(i, j int) bool { return g.profiles[i].LastUsed > g.profiles[j].LastUsed })
	g.profileIndex = 0
	for i, p := range g.profiles {
		if p.Name == name {
			g.profileIndex = i
			break
		}
	}
	g.candidateIndex = 0
	_ = g.store.Save(usageFile)
	g.syncLastUsed()

	g.flashText = "Copied: " + copiedText
	if len(g.flashText) > 25 {
		g.flashText = g.flashText[:25] + "…"
	}
	g.flashUntilMs = time.Now().UnixMilli() + 800
}

func (g *Game) goBack() {
	if g.view == "candidates" {
		g.view = "profiles"
		g.candidateIndex = 0
	}
}

func (g *Game) syncLastUsed() {
	for i := range g.profiles {
		p := &g.profiles[i]
		p.LastUsed = g.store.profileOrder(p.Name)
		for j := range p.Candidates {
			c := &p.Candidates[j]
			c.LastUsed = g.store.candidateOrder(p.Name, c.Text)
		}
	}
}

func (g *Game) rotateKnob(delta float64) {
	g.knobAngle += delta
	g.store.KnobAngle = g.knobAngle
	g.knobAccum += delta
	for g.knobAccum >= knobStepRad {
		g.moveHighlight(1)
		g.knobAccum -= knobStepRad
	}
	for g.knobAccum <= -knobStepRad {
		g.moveHighlight(-1)
		g.knobAccum += knobStepRad
	}
}

func (g *Game) moveHighlight(delta int) {
	changed := false
	if g.view == "profiles" {
		n := len(g.profiles)
		if n == 0 {
			return
		}
		old := g.profileIndex
		g.profileIndex = (g.profileIndex + delta + n) % n
		changed = g.profileIndex != old
	} else {
		prof := g.currentProfile()
		if prof == nil {
			return
		}
		n := len(prof.Candidates)
		if n == 0 {
			return
		}
		old := g.candidateIndex
		g.candidateIndex = (g.candidateIndex + delta + n) % n
		changed = g.candidateIndex != old
	}
	if changed && g.knobPressed {
		g.holdInvalidated = true
	}
}

func (g *Game) doShortPress() {
	if g.view == "profiles" {
		g.openCurrentProfile()
	} else {
		g.copyCurrentCandidate()
	}
}

func (g *Game) Update() error {
	// Keyboard navigation
	if inpututil.IsKeyJustPressed(ebiten.KeyArrowUp) || inpututil.IsKeyJustPressed(ebiten.KeyW) {
		g.rotateKnob(-knobStepRad)
		_ = g.store.Save(usageFile)
	}
	if inpututil.IsKeyJustPressed(ebiten.KeyArrowDown) || inpututil.IsKeyJustPressed(ebiten.KeyS) {
		g.rotateKnob(knobStepRad)
		_ = g.store.Save(usageFile)
	}
	if inpututil.IsKeyJustPressed(ebiten.KeyArrowLeft) {
		g.goBack()
	}
	if inpututil.IsKeyJustPressed(ebiten.KeyArrowRight) {
		g.doShortPress()
	}
	if inpututil.IsKeyJustPressed(ebiten.KeyEscape) || inpututil.IsKeyJustPressed(ebiten.KeyBackspace) {
		g.goBack()
	}
	if inpututil.IsKeyJustPressed(ebiten.KeyQ) {
		_ = g.store.Save(usageFile)
		return ebiten.Termination
	}

	// Enter / Space as knob press
	enterPressed := inpututil.IsKeyJustPressed(ebiten.KeyEnter) || inpututil.IsKeyJustPressed(ebiten.KeySpace) || inpututil.IsKeyJustPressed(ebiten.KeyNumpadEnter)
	enterReleased := inpututil.IsKeyJustReleased(ebiten.KeyEnter) || inpututil.IsKeyJustReleased(ebiten.KeySpace) || inpututil.IsKeyJustReleased(ebiten.KeyNumpadEnter)
	if enterPressed {
		g.knobPressed = true
		g.pressStartMs = time.Now().UnixMilli()
		g.longTriggered = false
		g.holdInvalidated = false
	}
	if enterReleased && g.knobPressed {
		duration := time.Now().UnixMilli() - g.pressStartMs
		g.knobPressed = false
		if duration < longPressMs && !g.longTriggered && !g.holdInvalidated {
			g.doShortPress()
		}
	}

	// Long press detection
	if g.knobPressed && !g.longTriggered && !g.holdInvalidated {
		if time.Now().UnixMilli()-g.pressStartMs >= longPressMs {
			g.longTriggered = true
			g.knobPressed = false
			g.goBack()
		}
	}

	// Mouse wheel
	_, wy := ebiten.Wheel()
	if wy != 0 {
		g.rotateKnob(-wy * knobStepRad)
		_ = g.store.Save(usageFile)
	}

	// Mouse interactions
	x, y := ebiten.CursorPosition()
	cx, cy := float64(x), float64(y)
	dist := math.Hypot(cx-knobCenterX, cy-knobCenterY)

	if inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonLeft) || inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonRight) {
		if dist <= float64(knobRadius) {
			g.mouseOnKnob = true
			g.dragStartX = cx
			g.dragStartY = cy
			g.isDragging = false
			g.lastDragAngle = math.Atan2(cy-knobCenterY, cx-knobCenterX)
			g.knobPressed = true
			g.pressStartMs = time.Now().UnixMilli()
			g.longTriggered = false
			g.holdInvalidated = false
		}
	}

	if g.mouseOnKnob {
		dx := cx - g.dragStartX
		dy := cy - g.dragStartY
		if !g.isDragging && math.Hypot(dx, dy) > 5 {
			g.isDragging = true
		}
		if g.isDragging {
			angle := math.Atan2(cy-knobCenterY, cx-knobCenterX)
			delta := angle - g.lastDragAngle
			for delta > math.Pi {
				delta -= 2 * math.Pi
			}
			for delta < -math.Pi {
				delta += 2 * math.Pi
			}
			g.lastDragAngle = angle
			g.rotateKnob(delta)
		}
	}

	if inpututil.IsMouseButtonJustReleased(ebiten.MouseButtonLeft) || inpututil.IsMouseButtonJustReleased(ebiten.MouseButtonRight) {
		if g.mouseOnKnob {
			g.mouseOnKnob = false
			g.knobPressed = false
			if g.isDragging {
				g.isDragging = false
				_ = g.store.Save(usageFile)
			} else {
				duration := time.Now().UnixMilli() - g.pressStartMs
				if duration < longPressMs && !g.longTriggered && !g.holdInvalidated {
					g.doShortPress()
				}
			}
		}
	}

	return nil
}

func (g *Game) Draw(screen *ebiten.Image) {
	screen.Fill(colorBG)

	// Device body
	vector.DrawFilledRect(screen, float32(deviceX), float32(deviceY), float32(deviceW), float32(deviceH), colorDevice, false)
	// Border would require stroke; simplified.

	// Display screen
	vector.DrawFilledRect(screen, float32(displayX), float32(displayY), float32(displayW), float32(displayH), colorDisplayBG, false)

	// Header
	header := "Profiles"
	if g.view == "candidates" {
		if prof := g.currentProfile(); prof != nil {
			header = prof.Name
		}
	}
	text.Draw(screen, header, g.fontTitle, displayX+12, displayY+18, colorText)

	// Separator line between header and list
	vector.StrokeLine(screen, float32(displayX+8), float32(displayY+30), float32(displayX+displayW-8), float32(displayY+30), 1, colorDisplayBorder, false)

	// List content
	var items []string
	var selected int
	if g.view == "profiles" {
		for _, p := range g.profiles {
			items = append(items, p.Name)
		}
		selected = g.profileIndex
	} else {
		prof := g.currentProfile()
		if prof != nil {
			for _, c := range prof.Candidates {
				items = append(items, c.Text)
			}
		}
		selected = g.candidateIndex
	}
	g.drawList(screen, items, selected, displayX+12, displayY+36, displayW-24, displayH-46)

	// Knob
	g.drawKnob(screen)

	// Flash overlay
	now := time.Now().UnixMilli()
	if now < g.flashUntilMs && g.flashText != "" {
		w := font.MeasureString(g.fontTitle, g.flashText).Round()
		h := g.fontTitle.Metrics().Height.Round()
		pad := 5
		cx := screenW / 2
		cy := displayY + displayH/2
		vector.DrawFilledRect(screen, float32(cx-w/2-pad), float32(cy-h/2-pad), float32(w+2*pad), float32(h+2*pad), color.NRGBA{230, 240, 200, 255}, false)
		text.Draw(screen, g.flashText, g.fontTitle, cx-w/2, cy-h/2+g.fontTitle.Metrics().Ascent.Round(), color.Black)
	}
}

func (g *Game) drawList(screen *ebiten.Image, items []string, selected, x, y, w, h int) {
	lineHeight := 17
	maxVisible := h / lineHeight
	ascent := g.fontBody.Metrics().Ascent.Round()
	if len(items) == 0 {
		text.Draw(screen, "(empty)", g.fontBody, x, y+ascent, colorTextDim)
		return
	}
	start := selected - maxVisible + 2
	if start < 0 {
		start = 0
	}
	end := start + maxVisible
	if end > len(items) {
		end = len(items)
	}
	for i := start; i < end; i++ {
		textItem := items[i]
		if len(textItem) > 33 {
			textItem = textItem[:32] + "…"
		}
		yy := y + (i-start)*lineHeight
		if i == selected {
			vector.DrawFilledRect(screen, float32(x-4), float32(yy-2), float32(w), float32(lineHeight), colorHighlight, false)
			text.Draw(screen, textItem, g.fontBody, x, yy+ascent, colorText)
		} else {
			text.Draw(screen, textItem, g.fontBody, x, yy+ascent, colorText)
		}
	}
}

func (g *Game) drawKnob(screen *ebiten.Image) {
	// Outer ring shadow
	vector.DrawFilledCircle(screen, float32(knobCenterX), float32(knobCenterY+4), float32(knobRadius), colorKnobShadow, false)
	// Outer ring
	vector.DrawFilledCircle(screen, float32(knobCenterX), float32(knobCenterY), float32(knobRadius), colorKnob, false)
	// Face
	faceColor := colorKnobFace
	if g.knobPressed {
		faceColor = colorHighlight
	}
	vector.DrawFilledCircle(screen, float32(knobCenterX), float32(knobCenterY), float32(knobInnerR), faceColor, false)
	// Indicator line
	ix := float64(knobCenterX) + float64(knobInnerR)*0.6*math.Cos(g.knobAngle)
	iy := float64(knobCenterY) + float64(knobInnerR)*0.6*math.Sin(g.knobAngle)
	vector.StrokeLine(screen, float32(knobCenterX), float32(knobCenterY), float32(ix), float32(iy), 4, colorIndicator, false)
}

func (g *Game) Layout(outsideWidth, outsideHeight int) (int, int) {
	return screenW, screenH
}

func main() {
	ebiten.SetWindowSize(screenW, screenH)
	ebiten.SetWindowTitle("Macro Keyboard Go Demo")
	ebiten.SetWindowResizingMode(ebiten.WindowResizingModeDisabled)
	ebiten.SetTPS(fps)

	game := newGame()
	if err := ebiten.RunGame(game); err != nil {
		log.Fatal(err)
	}
	// Final save on exit.
	_ = game.store.Save(usageFile)
	fmt.Println("Goodbye")
}
