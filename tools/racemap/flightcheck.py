import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build, verify
ctx = build.main([])
verify.flight_check(ctx)
