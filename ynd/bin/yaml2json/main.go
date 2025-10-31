package main

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"log"
	"os"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Println("Usage: yaml2json <input.yaml>")
		os.Exit(1)
	}

	inputFile := os.Args[1]

	yamlData, err := ioutil.ReadFile(inputFile)
	if err != nil {
		log.Fatalf("Error reading YAML file: %v", err)
	}

	var data interface{}
	err = Unmarshal(yamlData, &data)
	if err != nil {
		log.Fatalf("Error parsing YAML: %v", err)
	}

	jsonData, err := json.MarshalIndent(data, "", "  ")
	if err != nil {
		log.Fatalf("Error converting to JSON: %v", err)
	}

	fmt.Println(string(jsonData))
}

