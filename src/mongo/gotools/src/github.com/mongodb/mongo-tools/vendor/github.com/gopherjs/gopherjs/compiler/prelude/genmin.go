// +build ignore

package main

import (
	"bytes"
	"fmt"
	"go/build"
	"io/ioutil"
	"log"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/gopherjs/gopherjs/compiler/prelude"
)

func main() {
	if err := run(); err != nil {
		log.Fatalln(err)
	}
}

func run() error {
	bpkg, err := build.Import("github.com/gopherjs/gopherjs", "", build.FindOnly)
	if err != nil {
		return fmt.Errorf("failed to locate path for github.com/gopherjs/gopherjs: %v", err)
	}

	preludeDir := filepath.Join(bpkg.Dir, "compiler", "prelude")

	args := []string{
		filepath.Join(bpkg.Dir, "node_modules", ".bin", "uglifyjs"),
		"--config-file",
		filepath.Join(preludeDir, "uglifyjs_options.json"),
	}

	stderr := new(bytes.Buffer)
	cmd := exec.Command(args[0], args[1:]...)
	cmd.Stdin = strings.NewReader(prelude.Prelude)
	cmd.Stderr = stderr

	out, err := cmd.Output()
	if err != nil {
		return fmt.Errorf("failed to run %v: %v\n%s", strings.Join(args, " "), err, stderr.String())
	}

	fn := "prelude_min.go"

	outStr := fmt.Sprintf(`// Code generated by genmin; DO NOT EDIT.

package prelude

// Minified is an uglifyjs-minified version of Prelude.
const Minified = %q
`, out)

	if err := ioutil.WriteFile(fn, []byte(outStr), 0644); err != nil {
		return fmt.Errorf("failed to write to %v: %v", fn, err)
	}

	return nil
}
