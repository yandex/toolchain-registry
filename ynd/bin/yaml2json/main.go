package main

import (
	"encoding/json"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"os"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Println("Usage: yaml2json <input.yaml>")
		fmt.Println("If <input.yaml> is `-` then content reads from stdio")
		os.Exit(1)
	}

	inputFile := os.Args[1]
	yamlData := []byte{}
	if inputFile == "-" {
		data, err := io.ReadAll(os.Stdin)
		if err != nil {
			log.Fatalf("Error reading YAML from stdin: %v", err)
		}
		yamlData = data
	} else {
		data, err := ioutil.ReadFile(inputFile)
		if err != nil {
			log.Fatalf("Error reading YAML file: %v", err)
		}
		yamlData = data
	}

	var data interface{}
	err := Unmarshal(yamlData, &data)
	if err != nil {
		log.Fatalf("Error parsing YAML: %v", err)
	}

	jsonData, err := json.MarshalIndent(data, "", "  ")
	if err != nil {
		log.Fatalf("Error converting to JSON: %v", err)
	}

	fmt.Println(string(jsonData))
}
