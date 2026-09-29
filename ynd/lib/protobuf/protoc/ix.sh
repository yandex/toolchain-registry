{% extends '//lib/protobuf/t/ix.sh' %}

# The base lib/protobuf package disables protoc because library packages discard
# executables during IX post-processing. This companion host-tool package keeps
# protoc beside the protobuf library recipe so consumers can generate bindings
# with the exact protobuf toolchain used by the registry.

{% block cmake_flags %}
{{super()}}
protobuf_BUILD_PROTOC_BINARIES=ON
{% endblock %}
