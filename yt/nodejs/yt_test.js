var yt = require('./build/Release/yt_test');

var expect = require('chai').expect;
var assert = require('chai').assert;

describe("input stream interface", function() {
    before(function() {
        this.stream = new yt.TNodeJSInputStream();
        this.reader = new yt.TTestInputStream(this.stream);
    });

    it("should be able to read whole input byte-by-byte", function() {
        this.stream.Push(new Buffer("foo"), 0, 3);
        this.stream.Push(new Buffer("bar"), 0, 3);

        expect(this.reader.ReadSynchronously(1)).to.be.a("string").and.to.eql("f");
        expect(this.reader.ReadSynchronously(1)).to.be.a("string").and.to.eql("o");
        expect(this.reader.ReadSynchronously(1)).to.be.a("string").and.to.eql("o");
        expect(this.reader.ReadSynchronously(1)).to.be.a("string").and.to.eql("b");
        expect(this.reader.ReadSynchronously(1)).to.be.a("string").and.to.eql("a");
        expect(this.reader.ReadSynchronously(1)).to.be.a("string").and.to.eql("r");
    });

    it("should be able to read whole input at-a-time", function() {
        this.stream.Push(new Buffer("foo"), 0, 3);
        this.stream.Push(new Buffer("bar"), 0, 3);

        expect(this.reader.ReadSynchronously(6))
            .to.be.a("string").and.to.eql("foobar");
    });

    it("should be able to return if there is any data", function() {
        this.stream.Push(new Buffer("foo"), 0, 3);
        this.stream.Push(new Buffer("bar"), 0, 3);

        expect(this.reader.ReadSynchronously(1000))
            .to.be.a("string").and.to.eql("foobar");
    });

    it("should wait for new data while not closed", function(done) {
        this.stream.Push(new Buffer("foo"), 0, 3);
        this.stream.Push(new Buffer("bar"), 0, 3);

        var add_new_data = (function() {
            this.stream.Push(new Buffer("12345"), 0, 5);
        }).bind(this);
        var sweep_stream = (function() {
            this.stream.Sweep();
        }).bind(this);
        var close_stream = (function() {
            this.stream.Close();
        }).bind(this);

        setTimeout(add_new_data, 100);
        setTimeout(sweep_stream, 150);
        setTimeout(close_stream, 200);

        expect(this.reader.ReadSynchronously(3)).to.be.a("string").and.to.eql("foo");
        expect(this.reader.ReadSynchronously(3)).to.be.a("string").and.to.eql("bar");

        this.reader.Read(3, (function(length, buffer) {
            expect(length).to.be.eql(3);
            expect(buffer).to.be.equal("123");

            this.reader.Read(3, (function(length, buffer) {
                expect(length).to.be.eql(2);
                expect(buffer).to.be.equal("45");

                this.reader.Read(3, (function(length, buffer) {
                    expect(length).to.be.eql(0);
                    expect(buffer).to.be.empty;

                    done();
                }).bind(this));
            }).bind(this));
        }).bind(this));
    });

    it("should be able to read whole input if the stream was closed", function(done) {
        this.stream.Push(new Buffer("foo"), 0, 3);
        this.stream.Push(new Buffer("bar"), 0, 3);

        expect(this.reader.ReadSynchronously(2))
            .to.be.a("string").and.to.eql("fo");
        expect(this.reader.ReadSynchronously(2))
            .to.be.a("string").and.to.eql("ob");
        expect(this.reader.ReadSynchronously(2))
            .to.be.a("string").and.to.eql("ar");
        expect(this.reader.ReadSynchronously(2))
            .to.be.a("string").and.to.be.empty;
        expect(this.reader.ReadSynchronously(2))
            .to.be.a("string").and.to.be.empty;

        this.reader.Read(100, (function(length, buffer) {
            expect(length).to.be.eql(0);
            expect(buffer).to.be.empty;
            done();
        }).bind(this));
    });
});

describe("output stream interface", function() {
    before(function() {
        this.stream = new yt.TNodeJSOutputStream();
        this.writer = new yt.TTestOutputStream(this.stream);
    });

    it("should be able to write one chunk", function() {
        this.writer.WriteSynchronously("hello");

        expect(this.stream.Pull().toString())
            .to.be.equal("hello");
    });

    it("should be able to write two chunks", function() {
        this.writer.WriteSynchronously("hello");
        this.writer.WriteSynchronously("dolly");

        expect(this.stream.Pull().toString())
            .to.be.equal("hello");
        expect(this.stream.Pull().toString())
            .to.be.equal("dolly");
    });
});
