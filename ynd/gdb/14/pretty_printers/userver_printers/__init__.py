import gdb

import userver_printers.formats as formats

def register_printers():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("userver")
    formats.register_printers(pp)
    gdb.printing.register_pretty_printer(None, pp)
