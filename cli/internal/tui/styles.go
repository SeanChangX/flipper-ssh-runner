// SPDX-License-Identifier: GPL-3.0-only
package tui

import "github.com/charmbracelet/lipgloss"

// Flipper's colours: its orange (#FF8200) as the one accent, white text, and the screen's
// inverted look (black on orange) for what is selected. Every colour adapts: on a light
// terminal the orange darkens and the text turns near-black so both stay readable.
var (
	cAccent = lipgloss.AdaptiveColor{Light: "#C25E00", Dark: "#FF8200"}
	cOnAcc  = lipgloss.AdaptiveColor{Light: "#FFFFFF", Dark: "#000000"} // text on the orange
	cText   = lipgloss.AdaptiveColor{Light: "#1C1C1C", Dark: "#FFFFFF"}
	cDim    = lipgloss.AdaptiveColor{Light: "#6C6C6C", Dark: "#8A8A8A"}
	cRule   = lipgloss.AdaptiveColor{Light: "#D0D0D0", Dark: "#3A3A3A"}
	cGood   = lipgloss.AdaptiveColor{Light: "#2E7D32", Dark: "#7BD88F"}
	cWarn   = lipgloss.AdaptiveColor{Light: "#C25E00", Dark: "#FFB463"}
	cBad    = lipgloss.AdaptiveColor{Light: "#C62828", Dark: "#FF6B6B"}
)

// Spacing, as in Claude Code and other full-screen TUIs: a blank line above the header and
// below the key hints, two columns either side.
const (
	padTop    = 1
	padBottom = 1
	padX      = 2
)

var (
	sTitle    = lipgloss.NewStyle().Bold(true).Foreground(cText)
	sBrand    = lipgloss.NewStyle().Bold(true).Foreground(cAccent)
	sDim      = lipgloss.NewStyle().Foreground(cDim)
	sText     = lipgloss.NewStyle().Foreground(cText)
	sAccent   = lipgloss.NewStyle().Foreground(cAccent)
	sGood     = lipgloss.NewStyle().Foreground(cGood)
	sWarn     = lipgloss.NewStyle().Foreground(cWarn)
	sBad      = lipgloss.NewStyle().Foreground(cBad)
	sTabOn    = lipgloss.NewStyle().Bold(true).Foreground(cOnAcc).Background(cAccent).Padding(0, 2)
	sTabOff   = lipgloss.NewStyle().Foreground(cDim).Padding(0, 2)
	sRowOn    = lipgloss.NewStyle().Bold(true).Foreground(cOnAcc).Background(cAccent)
	sKey      = lipgloss.NewStyle().Foreground(cAccent).Bold(true)
	sHelp     = lipgloss.NewStyle().Foreground(cDim)
	sDialog   = lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).BorderForeground(cAccent).Padding(1, 3)
	sLabel    = lipgloss.NewStyle().Foreground(cDim).Width(17)
	sLabelOn  = lipgloss.NewStyle().Foreground(cAccent).Bold(true).Width(17)
	sFieldErr = lipgloss.NewStyle().Foreground(cBad)
	sBadge    = lipgloss.NewStyle().Foreground(cOnAcc).Background(cAccent).Bold(true).Padding(0, 1)
)
